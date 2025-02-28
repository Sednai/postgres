/*-------------------------------------------------------------------------
 *
 * pgxc_node.h
 *	  definition of the system "PGXC node" relation (pgxc_node)
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 * src/include/catalog/pgxc_node.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGXC_NODE_H
#define PGXC_NODE_H

#include "catalog/genbki.h"
#include "c.h"

#define PgxcNodeRelationId  9015

CATALOG(pgxc_node,9015,PgxcNodeRelationId) BKI_SHARED_RELATION
{
	Oid			oid;			/* oid */

	NameData	node_name;

	/*
	 * Possible node types are defined as follows
	 * Types are defined below PGXC_NODES_XXXX
	 */
	char		node_type;

	/*
	 * Port number of the node to connect to
	 */
	int32 		node_port;

	/*
	 * Host name of IP address of the node to connect to
	 */
	NameData	node_host;

	/*
	 * Is this node primary
	 */
	bool		nodeis_primary;

	/*
	 * Is this node preferred
	 */
	bool		nodeis_preferred;

	/*
	 * Node identifier to be used at places where a fixed length node identification is required
	 */
	int32		node_id;
} FormData_pgxc_node;

typedef FormData_pgxc_node *Form_pgxc_node;

#define Natts_pgxc_node				8
#define Anum_pgxc_node_oid			1
#define Anum_pgxc_node_name			2
#define Anum_pgxc_node_type			3
#define Anum_pgxc_node_port			4
#define Anum_pgxc_node_host			5
#define Anum_pgxc_node_is_primary	6
#define Anum_pgxc_node_is_preferred	7
#define Anum_pgxc_node_id			8

/* Possible types of nodes */
#define PGXC_NODE_COORDINATOR		'C'
#define PGXC_NODE_DATANODE			'D'
#define PGXC_NODE_NONE				'N'


#ifdef PGXC

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


#endif   /* PGXC_NODE_H */
