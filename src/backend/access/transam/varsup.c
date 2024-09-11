/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres OID & XID variables support routines
 *
 * Copyright (c) 2000-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/varsup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "utils/syscache.h"
#ifdef PGXC
#include "pgxc/pgxc.h"
#include "access/gtm.h"
#include "storage/procarray.h"
#endif


/* Number of OIDs to prefetch (preallocate) per XLOG write */
#define VAR_OID_PREFETCH		8192

/* pointer to "variable cache" in shared memory (set up by shmem.c) */
VariableCache ShmemVariableCache = NULL;


#ifdef PGXC  /* PGXC_DATANODE */
static FullTransactionId next_xid = {InvalidTransactionId};
static bool force_get_xid_from_gtm = false;

/*
 * Set next transaction id to use
 */
void
SetNextFullTransactionId(FullTransactionId xid)
{
	elog (DEBUG1, "[re]setting xid = %lu, old_value = %lu", xid.value, next_xid.value);
	next_xid = xid;
}

/*
 * Allow force of getting XID from GTM
 * Useful for explicit VACUUM (autovacuum already handled)
 */
void
SetForceXidFromGTM(bool value)
{
	force_get_xid_from_gtm = value;
}

/*
 * See if we should force using GTM
 * Useful for explicit VACUUM (autovacuum already handled)
 */
bool
GetForceXidFromGTM(void)
{
	return force_get_xid_from_gtm;
}
#endif /* PGXC */


/*
 * Allocate the next FullTransactionId for a new transaction or
 * subtransaction.
 *
 * The new XID is also stored into MyPgXact before returning.
 *
 * Note: when this is called, we are actually already inside a valid
 * transaction, since XIDs are now not allocated until the transaction
 * does something.  So it is safe to do a database lookup if we want to
 * issue a warning about XID wrap.
 */
FullTransactionId
#ifdef PGXC
GetNewTransactionId(bool isSubXact, bool *timestamp_received, GTM_Timestamp *timestamp)
#else
GetNewTransactionId(bool isSubXact)
#endif
{
	FullTransactionId full_xid;
	TransactionId xid;
#ifdef PGXC
	bool increment_xid = true;
	*timestamp_received = false;
#endif

	/*
	 * Workers synchronize transaction state at the beginning of each parallel
	 * operation, so we can't account for new XIDs after that point.
	 */
	if (IsInParallelMode())
		elog(ERROR, "cannot assign TransactionIds during a parallel operation");

	/*
	 * During bootstrap initialization, we return the special bootstrap
	 * transaction id.
	 */
	if (IsBootstrapProcessingMode())
	{
		Assert(!isSubXact);
		MyPgXact->xid = BootstrapTransactionId;
		return FullTransactionIdFromEpochAndXid(0, BootstrapTransactionId);
	}

	/* safety check, we should never get this far in a HS standby */
	if (RecoveryInProgress())
		elog(ERROR, "cannot assign TransactionIds during recovery");

#ifdef PGXC
	/* Initialize transaction ID */
	full_xid = InvalidFullTransactionId;
	xid = InvalidTransactionId;

	while (1)
	{
		increment_xid = true;
		*timestamp_received = false;
		/* if xid fall back, here we just commit the xid dummy and reget a new one from gtm */
		if (FullTransactionIdIsValid(full_xid))
		{
			CommitTranGTM(full_xid);
		}
		
		if ((IS_PGXC_COORDINATOR && !IsConnFromCoord()) || IsPGXCNodeXactDatanodeDirect())
		{
			/*
			 * Get XID from GTM before acquiring the lock as concurrent connections are
			 * being handled on GTM side even if the lock is acquired in a different
			 * order.
			 */
			if (IsAutoVacuumWorkerProcess() && (MyPgXact->vacuumFlags & PROC_IN_VACUUM))
				full_xid = BeginTranAutovacuumGTM();
				
			else
				full_xid = BeginTranGTM(timestamp);

			xid = XidFromFullTransactionId(full_xid);
			*timestamp_received = true;
		}
#endif

	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);

#ifdef PGXC
		/* Only remote Coordinator or a Datanode accessed directly by an application can get a GXID */
		if ((IS_PGXC_COORDINATOR && !IsConnFromCoord()) || IsPGXCNodeXactDatanodeDirect())
		{
			if (FullTransactionIdIsValid(full_xid))
			{
				/* Log some information about the new transaction ID obtained */
				if (IsAutoVacuumWorkerProcess() && (MyPgXact->vacuumFlags & PROC_IN_VACUUM))
					elog(DEBUG1, "Assigned new FullTransaction ID from GTM for autovacuum = %lu", full_xid.value);
				else
					elog(DEBUG1, "Assigned new FullTransaction ID from GTM = %lu", full_xid.value);
				
				if (!FullTransactionIdFollowsOrEquals(full_xid, ShmemVariableCache->nextFullXid))
				{
					increment_xid = false;
					LWLockRelease(XidGenLock);
					
					/* It is a serious issue when xid fall back, so here we write a message to record this */
					ereport(DEBUG1,
					   (errmsg("full_xid (%lu) was less than ShmemVariableCache->nextFullXid (%lu), IS_PGXC_COORDINATOR:%d, IS_PGXC_DATANODE:%d, REMOTE_CONN_TYPE:%d, regen now",
						   full_xid.value, ShmemVariableCache->nextFullXid.value, IS_PGXC_COORDINATOR, IS_PGXC_DATANODE, REMOTE_CONN_TYPE)));

					/* here, we get a fallback xid, reget a new one to avoid data corruption */
					continue;					 
				}
				else
					ShmemVariableCache->nextFullXid = full_xid;
			}
			else
			{
				ereport(WARNING,
				   (errmsg("FullXid is invalid.")));

				/* Problem is already reported, so just remove lock and return */
				LWLockRelease(XidGenLock);
				return full_xid;
			}
		}
		else if(IS_PGXC_DATANODE || IsConnFromCoord())
		{
			if (IsAutoVacuumWorkerProcess())
			{
				/*
				 * For an autovacuum worker process, get transaction ID directly from GTM.
				 * If this vacuum process is a vacuum analyze, its GXID has to be excluded
				 * from snapshots so use a special function for this purpose.
				 * For a simple worker get transaction ID like a normal transaction would do.
				 */
				if (MyPgXact->vacuumFlags & PROC_IN_VACUUM)
					next_xid = BeginTranAutovacuumGTM();
				else
					next_xid = BeginTranGTM(timestamp);
			}
			else if (GetForceXidFromGTM())
			{
				elog (DEBUG1, "Force get XID from GTM");
				/* try and get gxid directly from GTM */
				next_xid =  BeginTranGTM(NULL);
			}

			if (FullTransactionIdIsValid(next_xid))
			{
				full_xid = next_xid;
				xid = XidFromFullTransactionId(full_xid);
				elog(DEBUG1, "TransactionId = %lu", next_xid.value);
				next_xid = InvalidFullTransactionId; /* reset */
				if (!FullTransactionIdFollowsOrEquals(full_xid, ShmemVariableCache->nextFullXid))
				{
					/* This should be ok, due to concurrency from multiple coords
					 * passing down the xids.
					 * We later do not want to bother incrementing the value
					 * in shared memory though.
					 */
					increment_xid = false;
					elog(DEBUG1, "xid (%lu) does not follow ShmemVariableCache->nextXid (%lu)",
						full_xid.value, ShmemVariableCache->nextFullXid.value);
				}
				else
					ShmemVariableCache->nextFullXid = full_xid;
			}
			else
			{	
				/* Fallback to default */
				if (!useLocalXid)
					elog(ERROR, "Falling back to local Xid. Was = %lu, now is = %lu",
						next_xid.value, ShmemVariableCache->nextFullXid.value);
				full_xid = ShmemVariableCache->nextFullXid;
				xid = XidFromFullTransactionId(full_xid);
			}
		}

		/* successfully get a new xid */
		break;
	}
#else
	full_xid = ShmemVariableCache->nextFullXid;
	xid = XidFromFullTransactionId(full_xid);
#endif /* PGXC */


	/*----------
	 * Check to see if it's safe to assign another XID.  This protects against
	 * catastrophic data loss due to XID wraparound.  The basic rules are:
	 *
	 * If we're past xidVacLimit, start trying to force autovacuum cycles.
	 * If we're past xidWarnLimit, start issuing warnings.
	 * If we're past xidStopLimit, refuse to execute transactions, unless
	 * we are running in single-user mode (which gives an escape hatch
	 * to the DBA who somehow got past the earlier defenses).
	 *
	 * Note that this coding also appears in GetNewMultiXactId.
	 *----------
	 */
#ifdef PGXC
	/*
	 * In PG, the xid will never cross the wrap-around limit thanks to the
	 * above checks. But in PGXC, if a new node is initialized and brought up,
	 * it's own xid may not be in sync with the GTM gxid. The wrap-around limits
	 * are initially set w.r.t. the last xid used. So if the Gxid-xid difference
	 * is already more than 2^31, then the gxid is deemed to have already
	 * crossed the wrap-around limit. So again in such cases as well, we should
	 * allow only a standalone backend to run vacuum.
	 */
	if (TransactionIdFollowsOrEquals(xid, ShmemVariableCache->xidWrapLimit))
	{
		if (IsPostmasterEnvironment)
		    ereport(ERROR,
		       (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
		       errmsg("Xid wraparound might have already happened. database is not accepting commands on database with OID %u",
		       ShmemVariableCache->oldestXidDB),
		       errhint("Stop the postmaster and use a standalone backend to vacuum that database.\n"
		               "You might also need to commit or roll back old prepared transactions.")));
	}
	else
#endif
	if (TransactionIdFollowsOrEquals(xid, ShmemVariableCache->xidVacLimit))
	{
		/*
		 * For safety's sake, we release XidGenLock while sending signals,
		 * warnings, etc.  This is not so much because we care about
		 * preserving concurrency in this situation, as to avoid any
		 * possibility of deadlock while doing get_database_name(). First,
		 * copy all the shared values we'll need in this path.
		 */
		TransactionId xidWarnLimit = ShmemVariableCache->xidWarnLimit;
		TransactionId xidStopLimit = ShmemVariableCache->xidStopLimit;
		TransactionId xidWrapLimit = ShmemVariableCache->xidWrapLimit;
		Oid			oldest_datoid = ShmemVariableCache->oldestXidDB;

		LWLockRelease(XidGenLock);

		/*
		 * To avoid swamping the postmaster with signals, we issue the autovac
		 * request only once per 64K transaction starts.  This still gives
		 * plenty of chances before we get into real trouble.
		 */
		if (IsUnderPostmaster && (xid % 65536) == 0)
			SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

		if (IsUnderPostmaster &&
			TransactionIdFollowsOrEquals(xid, xidStopLimit))
		{
#ifdef PGXC
			/*
			 * Allow auto-vacuum to carry-on, so that it gets a chance to correct
			 * the xid-wrap-limits w.r.t to gxid fetched from GTM.
			 */
			if (!IsAutoVacuumLauncherProcess() && !IsAutoVacuumWorkerProcess())
			{
				char  *oldest_datname = (OidIsValid(MyDatabaseId) ?
			           get_database_name(oldest_datoid): NULL);
#else
			char	   *oldest_datname = get_database_name(oldest_datoid);
#endif
			/* complain even if that DB has disappeared */
			if (oldest_datname)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands to avoid wraparound data loss in database \"%s\"",
								oldest_datname),
						 errhint("Stop the postmaster and vacuum that database in single-user mode.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands to avoid wraparound data loss in database with OID %u",
								oldest_datoid),
						 errhint("Stop the postmaster and vacuum that database in single-user mode.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
#ifdef PGXC
			}
#endif
		}
		else if (TransactionIdFollowsOrEquals(xid, xidWarnLimit))
		{
			char	   *oldest_datname = get_database_name(oldest_datoid);

			/* complain even if that DB has disappeared */
			if (oldest_datname)
				ereport(WARNING,
						(errmsg("database \"%s\" must be vacuumed within %u transactions",
								oldest_datname,
								xidWrapLimit - xid),
						 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
			else
				ereport(WARNING,
						(errmsg("database with OID %u must be vacuumed within %u transactions",
								oldest_datoid,
								xidWrapLimit - xid),
						 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		}

		/* Re-acquire lock and start over */
		LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
#ifndef PGXC
		/*
		 * In the case of Postgres-XC, transaction ID is managed globally at GTM level,
		 * so updating the GXID here based on the cache that might have been changed
		 * by another session when checking for wraparound errors at this local node
		 * level breaks transaction ID consistency of cluster.
		 */
		full_xid = ShmemVariableCache->nextFullXid;
		xid = XidFromFullTransactionId(full_xid);
#endif
	}

	/*
	 * If we are allocating the first XID of a new page of the commit log,
	 * zero out that commit-log page before returning. We must do this while
	 * holding XidGenLock, else another xact could acquire and commit a later
	 * XID before we zero the page.  Fortunately, a page of the commit log
	 * holds 32K or more transactions, so we don't have to do this very often.
	 *
	 * Extend pg_subtrans and pg_commit_ts too.
	 */
	ExtendCLOG(xid);
	ExtendCommitTs(xid);
	ExtendSUBTRANS(xid);

	/*
	 * Now advance the nextFullXid counter.  This must not happen until after
	 * we have successfully completed ExtendCLOG() --- if that routine fails,
	 * we want the next incoming transaction to try it again.  We cannot
	 * assign more XIDs until there is CLOG space for them.
	 */
#ifdef PGXC
	/*
	 * But first bring nextXid in sync with global xid. Actually we get xid
	 * externally anyway, so it should not be needed to update nextXid in
	 * theory, but it is required to keep nextXid close to the gxid
	 * especially when vacuumfreeze is run using a standalone backend.
	 */
	if (increment_xid || !IsPostmasterEnvironment)
	{
		ShmemVariableCache->nextFullXid = full_xid;
	}
#endif
	FullTransactionIdAdvance(&ShmemVariableCache->nextFullXid);

	/*
	 * We must store the new XID into the shared ProcArray before releasing
	 * XidGenLock.  This ensures that every active XID older than
	 * latestCompletedXid is present in the ProcArray, which is essential for
	 * correct OldestXmin tracking; see src/backend/access/transam/README.
	 *
	 * Note that readers of PGXACT xid fields should be careful to fetch the
	 * value only once, rather than assume they can read a value multiple
	 * times and get the same answer each time.  Note we are assuming that
	 * TransactionId and int fetch/store are atomic.
	 *
	 * The same comments apply to the subxact xid count and overflow fields.
	 *
	 * Use of a write barrier prevents dangerous code rearrangement in this
	 * function; other backends could otherwise e.g. be examining my subxids
	 * info concurrently, and we don't want them to see an invalid
	 * intermediate state, such as an incremented nxids before the array entry
	 * is filled.
	 *
	 * Other processes that read nxids should do so before reading xids
	 * elements with a pg_read_barrier() in between, so that they can be sure
	 * not to read an uninitialized array element; see
	 * src/backend/storage/lmgr/README.barrier.
	 *
	 * If there's no room to fit a subtransaction XID into PGPROC, set the
	 * cache-overflowed flag instead.  This forces readers to look in
	 * pg_subtrans to map subtransaction XIDs up to top-level XIDs. There is a
	 * race-condition window, in that the new XID will not appear as running
	 * until its parent link has been placed into pg_subtrans. However, that
	 * will happen before anyone could possibly have a reason to inquire about
	 * the status of the XID, so it seems OK.  (Snapshots taken during this
	 * window *will* include the parent XID, so they will deliver the correct
	 * answer later on when someone does have a reason to inquire.)
	 */
	if (!isSubXact)
		MyPgXact->xid = xid;	/* LWLockRelease acts as barrier */
	else
	{
		int			nxids = MyPgXact->nxids;

		if (nxids < PGPROC_MAX_CACHED_SUBXIDS)
		{
			MyProc->subxids.xids[nxids] = xid;
			pg_write_barrier();
			MyPgXact->nxids = nxids + 1;
		}
		else
			MyPgXact->overflowed = true;
	}

	LWLockRelease(XidGenLock);

	return full_xid;
}

/*
 * Read nextFullXid but don't allocate it.
 */
FullTransactionId
ReadNextFullTransactionId(void)
{
	FullTransactionId fullXid;

	LWLockAcquire(XidGenLock, LW_SHARED);
	fullXid = ShmemVariableCache->nextFullXid;
	LWLockRelease(XidGenLock);

	return fullXid;
}

/*
 * Advance nextFullXid to the value after a given xid.  The epoch is inferred.
 * This must only be called during recovery or from two-phase start-up code.
 */
void
AdvanceNextFullTransactionIdPastXid(TransactionId xid)
{
	FullTransactionId newNextFullXid;
	TransactionId next_xid;
	uint32		epoch;

	/*
	 * It is safe to read nextFullXid without a lock, because this is only
	 * called from the startup process or single-process mode, meaning that no
	 * other process can modify it.
	 */
	Assert(AmStartupProcess() || !IsUnderPostmaster);

	/* Fast return if this isn't an xid high enough to move the needle. */
	next_xid = XidFromFullTransactionId(ShmemVariableCache->nextFullXid);
	if (!TransactionIdFollowsOrEquals(xid, next_xid))
		return;

	/*
	 * Compute the FullTransactionId that comes after the given xid.  To do
	 * this, we preserve the existing epoch, but detect when we've wrapped
	 * into a new epoch.  This is necessary because WAL records and 2PC state
	 * currently contain 32 bit xids.  The wrap logic is safe in those cases
	 * because the span of active xids cannot exceed one epoch at any given
	 * point in the WAL stream.
	 */
	TransactionIdAdvance(xid);
	epoch = EpochFromFullTransactionId(ShmemVariableCache->nextFullXid);
	if (unlikely(xid < next_xid))
		++epoch;
	newNextFullXid = FullTransactionIdFromEpochAndXid(epoch, xid);

	/*
	 * We still need to take a lock to modify the value when there are
	 * concurrent readers.
	 */
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
	ShmemVariableCache->nextFullXid = newNextFullXid;
	LWLockRelease(XidGenLock);
}

/*
 * Advance the cluster-wide value for the oldest valid clog entry.
 *
 * We must acquire CLogTruncationLock to advance the oldestClogXid. It's not
 * necessary to hold the lock during the actual clog truncation, only when we
 * advance the limit, as code looking up arbitrary xids is required to hold
 * CLogTruncationLock from when it tests oldestClogXid through to when it
 * completes the clog lookup.
 */
void
AdvanceOldestClogXid(TransactionId oldest_datfrozenxid)
{
	LWLockAcquire(CLogTruncationLock, LW_EXCLUSIVE);
	if (TransactionIdPrecedes(ShmemVariableCache->oldestClogXid,
							  oldest_datfrozenxid))
	{
		ShmemVariableCache->oldestClogXid = oldest_datfrozenxid;
	}
	LWLockRelease(CLogTruncationLock);
}

/*
 * Determine the last safe XID to allocate using the currently oldest
 * datfrozenxid (ie, the oldest XID that might exist in any database
 * of our cluster), and the OID of the (or a) database with that value.
 */
void
SetTransactionIdLimit(TransactionId oldest_datfrozenxid, Oid oldest_datoid)
{
	TransactionId xidVacLimit;
	TransactionId xidWarnLimit;
	TransactionId xidStopLimit;
	TransactionId xidWrapLimit;
	TransactionId curXid;

	Assert(TransactionIdIsNormal(oldest_datfrozenxid));

	/*
	 * The place where we actually get into deep trouble is halfway around
	 * from the oldest potentially-existing XID.  (This calculation is
	 * probably off by one or two counts, because the special XIDs reduce the
	 * size of the loop a little bit.  But we throw in plenty of slop below,
	 * so it doesn't matter.)
	 */
	xidWrapLimit = oldest_datfrozenxid + (MaxTransactionId >> 1);
	if (xidWrapLimit < FirstNormalTransactionId)
		xidWrapLimit += FirstNormalTransactionId;

	/*
	 * We'll refuse to continue assigning XIDs in interactive mode once we get
	 * within 1M transactions of data loss.  This leaves lots of room for the
	 * DBA to fool around fixing things in a standalone backend, while not
	 * being significant compared to total XID space. (Note that since
	 * vacuuming requires one transaction per table cleaned, we had better be
	 * sure there's lots of XIDs left...)
	 */
	xidStopLimit = xidWrapLimit - 1000000;
	if (xidStopLimit < FirstNormalTransactionId)
		xidStopLimit -= FirstNormalTransactionId;

	/*
	 * We'll start complaining loudly when we get within 10M transactions of
	 * the stop point.  This is kind of arbitrary, but if you let your gas
	 * gauge get down to 1% of full, would you be looking for the next gas
	 * station?  We need to be fairly liberal about this number because there
	 * are lots of scenarios where most transactions are done by automatic
	 * clients that won't pay attention to warnings. (No, we're not gonna make
	 * this configurable.  If you know enough to configure it, you know enough
	 * to not get in this kind of trouble in the first place.)
	 */
	xidWarnLimit = xidStopLimit - 10000000;
	if (xidWarnLimit < FirstNormalTransactionId)
		xidWarnLimit -= FirstNormalTransactionId;

	/*
	 * We'll start trying to force autovacuums when oldest_datfrozenxid gets
	 * to be more than autovacuum_freeze_max_age transactions old.
	 *
	 * Note: guc.c ensures that autovacuum_freeze_max_age is in a sane range,
	 * so that xidVacLimit will be well before xidWarnLimit.
	 *
	 * Note: autovacuum_freeze_max_age is a PGC_POSTMASTER parameter so that
	 * we don't have to worry about dealing with on-the-fly changes in its
	 * value.  It doesn't look practical to update shared state from a GUC
	 * assign hook (too many processes would try to execute the hook,
	 * resulting in race conditions as well as crashes of those not connected
	 * to shared memory).  Perhaps this can be improved someday.  See also
	 * SetMultiXactIdLimit.
	 */
	xidVacLimit = oldest_datfrozenxid + autovacuum_freeze_max_age;
	if (xidVacLimit < FirstNormalTransactionId)
		xidVacLimit += FirstNormalTransactionId;

	/* Grab lock for just long enough to set the new limit values */
	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
	ShmemVariableCache->oldestXid = oldest_datfrozenxid;
	ShmemVariableCache->xidVacLimit = xidVacLimit;
	ShmemVariableCache->xidWarnLimit = xidWarnLimit;
	ShmemVariableCache->xidStopLimit = xidStopLimit;
	ShmemVariableCache->xidWrapLimit = xidWrapLimit;
	ShmemVariableCache->oldestXidDB = oldest_datoid;
	curXid = XidFromFullTransactionId(ShmemVariableCache->nextFullXid);
	LWLockRelease(XidGenLock);

	/* Log the info */
	ereport(DEBUG1,
			(errmsg("transaction ID wrap limit is %u, limited by database with OID %u",
					xidWrapLimit, oldest_datoid)));

	/*
	 * If past the autovacuum force point, immediately signal an autovac
	 * request.  The reason for this is that autovac only processes one
	 * database per invocation.  Once it's finished cleaning up the oldest
	 * database, it'll call here, and we'll signal the postmaster to start
	 * another iteration immediately if there are still any old databases.
	 */
	if (TransactionIdFollowsOrEquals(curXid, xidVacLimit) &&
		IsUnderPostmaster && !InRecovery)
		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

	/* Give an immediate warning if past the wrap warn point */
	if (TransactionIdFollowsOrEquals(curXid, xidWarnLimit) && !InRecovery)
	{
		char	   *oldest_datname;

		/*
		 * We can be called when not inside a transaction, for example during
		 * StartupXLOG().  In such a case we cannot do database access, so we
		 * must just report the oldest DB's OID.
		 *
		 * Note: it's also possible that get_database_name fails and returns
		 * NULL, for example because the database just got dropped.  We'll
		 * still warn, even though the warning might now be unnecessary.
		 */
		if (IsTransactionState())
			oldest_datname = get_database_name(oldest_datoid);
		else
			oldest_datname = NULL;

		if (oldest_datname)
			ereport(WARNING,
					(errmsg("database \"%s\" must be vacuumed within %u transactions",
							oldest_datname,
							xidWrapLimit - curXid),
					 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		else
			ereport(WARNING,
					(errmsg("database with OID %u must be vacuumed within %u transactions",
							oldest_datoid,
							xidWrapLimit - curXid),
					 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
	}
}


/*
 * ForceTransactionIdLimitUpdate -- does the XID wrap-limit data need updating?
 *
 * We primarily check whether oldestXidDB is valid.  The cases we have in
 * mind are that that database was dropped, or the field was reset to zero
 * by pg_resetwal.  In either case we should force recalculation of the
 * wrap limit.  Also do it if oldestXid is old enough to be forcing
 * autovacuums or other actions; this ensures we update our state as soon
 * as possible once extra overhead is being incurred.
 */
bool
ForceTransactionIdLimitUpdate(void)
{
	TransactionId nextXid;
	TransactionId xidVacLimit;
	TransactionId oldestXid;
	Oid			oldestXidDB;

	/* Locking is probably not really necessary, but let's be careful */
	LWLockAcquire(XidGenLock, LW_SHARED);
	nextXid = XidFromFullTransactionId(ShmemVariableCache->nextFullXid);
	xidVacLimit = ShmemVariableCache->xidVacLimit;
	oldestXid = ShmemVariableCache->oldestXid;
	oldestXidDB = ShmemVariableCache->oldestXidDB;
	LWLockRelease(XidGenLock);

	if (!TransactionIdIsNormal(oldestXid))
		return true;			/* shouldn't happen, but just in case */
	if (!TransactionIdIsValid(xidVacLimit))
		return true;			/* this shouldn't happen anymore either */
	if (TransactionIdFollowsOrEquals(nextXid, xidVacLimit))
		return true;			/* past VacLimit, don't delay updating */
	if (!SearchSysCacheExists1(DATABASEOID, ObjectIdGetDatum(oldestXidDB)))
		return true;			/* could happen, per comments above */
	return false;
}


/*
 * GetNewObjectId -- allocate a new OID
 *
 * OIDs are generated by a cluster-wide counter.  Since they are only 32 bits
 * wide, counter wraparound will occur eventually, and therefore it is unwise
 * to assume they are unique unless precautions are taken to make them so.
 * Hence, this routine should generally not be used directly.  The only direct
 * callers should be GetNewOidWithIndex() and GetNewRelFileNode() in
 * catalog/catalog.c.
 */
Oid
GetNewObjectId(void)
{
	Oid			result;

	/* safety check, we should never get this far in a HS standby */
	if (RecoveryInProgress())
		elog(ERROR, "cannot assign OIDs during recovery");

	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

	/*
	 * Check for wraparound of the OID counter.  We *must* not return 0
	 * (InvalidOid), and in normal operation we mustn't return anything below
	 * FirstNormalObjectId since that range is reserved for initdb (see
	 * IsCatalogRelationOid()).  Note we are relying on unsigned comparison.
	 *
	 * During initdb, we start the OID generator at FirstBootstrapObjectId, so
	 * we only wrap if before that point when in bootstrap or standalone mode.
	 * The first time through this routine after normal postmaster start, the
	 * counter will be forced up to FirstNormalObjectId.  This mechanism
	 * leaves the OIDs between FirstBootstrapObjectId and FirstNormalObjectId
	 * available for automatic assignment during initdb, while ensuring they
	 * will never conflict with user-assigned OIDs.
	 */
	if (ShmemVariableCache->nextOid < ((Oid) FirstNormalObjectId))
	{
		if (IsPostmasterEnvironment)
		{
			/* wraparound, or first post-initdb assignment, in normal mode */
			ShmemVariableCache->nextOid = FirstNormalObjectId;
			ShmemVariableCache->oidCount = 0;
		}
		else
		{
			/* we may be bootstrapping, so don't enforce the full range */
			if (ShmemVariableCache->nextOid < ((Oid) FirstBootstrapObjectId))
			{
				/* wraparound in standalone mode (unlikely but possible) */
				ShmemVariableCache->nextOid = FirstNormalObjectId;
				ShmemVariableCache->oidCount = 0;
			}
		}
	}

	/* If we run out of logged for use oids then we must log more */
	if (ShmemVariableCache->oidCount == 0)
	{
		XLogPutNextOid(ShmemVariableCache->nextOid + VAR_OID_PREFETCH);
		ShmemVariableCache->oidCount = VAR_OID_PREFETCH;
	}

	result = ShmemVariableCache->nextOid;

	(ShmemVariableCache->nextOid)++;
	(ShmemVariableCache->oidCount)--;

	LWLockRelease(OidGenLock);

	return result;
}