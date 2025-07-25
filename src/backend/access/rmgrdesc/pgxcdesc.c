/*-------------------------------------------------------------------------
 *
 * pgxcdesc.c
 *	  rmgr descriptor routines for XC special
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 2010-2014 Postgres-XC Development Group
 *
 * IDENTIFICATION
 *	  $$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgxc/barrier.h"
void
barrier_redo(XLogReaderState *record)
{
	/* Nothing to do */
	return;
}

void
barrier_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	appendStringInfo(buf, "BARRIER %s", rec);
}

const char *
barrier_identify(uint8 info)
{
	return "Barrier";
}