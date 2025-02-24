/*-------------------------------------------------------------------------
 *
 * indexing.h
 *	  This file provides some definitions to support indexing
 *	  on system catalogs
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 * 
 * src/include/catalog/indexing.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEXING_H
#define INDEXING_H

#include "access/htup.h"
#include "nodes/execnodes.h"
#include "utils/relcache.h"

/*
 * The state object used by CatalogOpenIndexes and friends is actually the
 * same as the executor's ResultRelInfo, but we give it another type name
 * to decouple callers from that fact.
 */
typedef struct ResultRelInfo *CatalogIndexState;

/*
 * Cap the maximum amount of bytes allocated for multi-inserts with system
 * catalogs, limiting the number of slots used.
 */
#define MAX_CATALOG_MULTI_INSERT_BYTES 65535

/*
 * indexing.c prototypes
 */
extern CatalogIndexState CatalogOpenIndexes(Relation heapRel);
extern void CatalogCloseIndexes(CatalogIndexState indstate);
extern void CatalogTupleInsert(Relation heapRel, HeapTuple tup);
extern void CatalogTupleInsertWithInfo(Relation heapRel, HeapTuple tup,
									   CatalogIndexState indstate);
extern void CatalogTuplesMultiInsertWithInfo(Relation heapRel,
											 TupleTableSlot **slot,
											 int ntuples,
											 CatalogIndexState indstate);
extern void CatalogTupleUpdate(Relation heapRel, ItemPointer otid,
							   HeapTuple tup);
extern void CatalogTupleUpdateWithInfo(Relation heapRel,
									   ItemPointer otid, HeapTuple tup,
									   CatalogIndexState indstate);
extern void CatalogTupleDelete(Relation heapRel, ItemPointer tid);

#ifdef PGXC
extern void CatalogIndexInsert(CatalogIndexState indstate, HeapTuple heapTuple);

#define PgxcClassPgxcRelIdIndexId 	9002
DECLARE_UNIQUE_INDEX(pgxc_class_pcrelid_index, 9002, PgxcClassPgxcRelIdIndexId, on pgxc_class using btree(pcrelid oid_ops));

#define PgxcNodeOidIndexId			9010
DECLARE_UNIQUE_INDEX(pgxc_node_oid_index, 9010, PgxcNodeOidIndexId, on pgxc_node using btree(oid oid_ops));

#define PgxcNodeNodeNameIndexId 	9011
DECLARE_UNIQUE_INDEX(pgxc_node_name_index, 9011, PgxcNodeNodeNameIndexId, on pgxc_node using btree(node_name name_ops));

#define PgxcGroupGroupNameIndexId 	9012
DECLARE_UNIQUE_INDEX(pgxc_group_name_index, 9012, PgxcGroupGroupNameIndexId, on pgxc_group using btree(group_name name_ops));

#define PgxcGroupOidIndexId			9013
DECLARE_UNIQUE_INDEX(pgxc_group_oid, 9013, PgxcGroupOidIndexId, on pgxc_group using btree(oid oid_ops));

#define PgxcNodeNodeIdIndexId 	9003
DECLARE_UNIQUE_INDEX(pgxc_node_id_index, 9003, PgxcNodeNodeIdIndexId, on pgxc_node using btree(node_id int4_ops));


#endif

#endif							/* INDEXING_H */
