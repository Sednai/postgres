/*-------------------------------------------------------------------------
 *
 * gtm.h
 *
 *	  Module interfacing with GTM definitions
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef ACCESS_GTM_H
#define ACCESS_GTM_H

#include "gtm/gtm_c.h"
#include "access/transam.h"

/* Configuration variables */
extern char *GtmHost;
extern int GtmPort;
extern bool gtm_backup_barrier;

extern FullTransactionId currentGxid;

extern bool IsGTMConnected(void);
extern void InitGTM(void);
extern void CloseGTM(void);
extern FullTransactionId BeginTranGTM(GTM_Timestamp *timestamp);
extern FullTransactionId BeginTranAutovacuumGTM(void);
extern int CommitTranGTM(FullTransactionId gxid);
extern int RollbackTranGTM(FullTransactionId gxid);
extern int StartPreparedTranGTM(FullTransactionId gxid,
								char *gid,
								char *nodestring);
extern int PrepareTranGTM(FullTransactionId gxid);
extern int GetGIDDataGTM(char *gid,
						 FullTransactionId *gxid,
						 FullTransactionId *prepared_gxid,
						 char **nodestring);
extern int CommitPreparedTranGTM(FullTransactionId gxid,
								 FullTransactionId prepared_gxid);

extern GTM_Snapshot GetSnapshotGTM(FullTransactionId gxid, bool canbe_grouped);

/* Node registration APIs with GTM */
extern int RegisterGTM(GTM_PGXCNodeType type, GTM_PGXCNodePort port, const char *datafolder);
extern int UnregisterGTM(GTM_PGXCNodeType type);

/* Sequence interface APIs with GTM */
extern GTM_Sequence GetNextValGTM(char *seqname);
extern int SetValGTM(char *seqname, GTM_Sequence nextval, bool iscalled);
extern int CreateSequenceGTM(char *seqname, GTM_Sequence increment,
		GTM_Sequence minval, GTM_Sequence maxval, GTM_Sequence startval,
		bool cycle);
extern int AlterSequenceGTM(char *seqname, GTM_Sequence increment,
		GTM_Sequence minval, GTM_Sequence maxval, GTM_Sequence startval,
							GTM_Sequence lastval, bool cycle, bool is_restart);
extern int DropSequenceGTM(char *name, GTM_SequenceKeyType type);
extern int RenameSequenceGTM(char *seqname, const char *newseqname);
/* Barrier */
extern int ReportBarrierGTM(char *barrier_id);
#endif /* ACCESS_GTM_H */
