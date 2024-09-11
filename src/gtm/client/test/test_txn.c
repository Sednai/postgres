/*
 * Copyright (c) 2010-2012 Postgres-XC Development Group
 */

#include <sys/types.h>
#include <unistd.h>

#include "gtm/gtm_c.h"
#include "gtm/libpq-fe.h"
#include "gtm/gtm_client.h"

#define client_log(x)	printf x

int
main(int argc, char *argv[])
{
	int ii;
	FullTransactionId gxid[4000];
	GTM_Conn *conn;
	char connect_string[100];
	GTM_Timestamp *timestamp;

	for (ii = 0; ii < 3; ii++)
		fork();

	sprintf(connect_string, "host=localhost port=6666 node_name=one remote_type=%d", GTM_NODE_COORDINATOR);

	conn = PQconnectGTM(connect_string);
	if (conn == NULL)
	{
		client_log(("Error in connection\n"));
		exit(1);
	}

	for (ii = 0; ii < 20; ii++)
	{
		gxid[ii] = begin_transaction(conn, GTM_ISOLATION_SERIALIZABLE, timestamp);
		if (!FullTransactionIdIsValid(gxid[ii]))
			client_log(("Started a new transaction (GXID:%lu)\n", gxid[ii].value));
		else
			client_log(("BEGIN transaction failed for ii=%d\n", ii));
	}

	for (ii = 0; ii < 20; ii++)
	{
		if (!prepare_transaction(conn, gxid[ii]))
			client_log(("PREPARE successful (GXID:%lu)\n", gxid[ii].value));
		else
			client_log(("PREPARE failed (GXID:%lu)\n", gxid[ii].value));
	}

	for (ii = 0; ii < 20; ii++)
	{
		if (ii % 2 == 0)
		{
			if (!abort_transaction(conn, gxid[ii]))
				client_log(("ROLLBACK successful (GXID:%lu)\n", gxid[ii].value));
			else
				client_log(("ROLLBACK failed (GXID:%lu)\n", gxid[ii].value));
		}
		else
		{
			if (!commit_transaction(conn, gxid[ii]))
				client_log(("COMMIT successful (GXID:%lu)\n", gxid[ii].value));
			else
				client_log(("COMMIT failed (GXID:%lu)\n", gxid[ii].value));
		}
	}

	GTMPQfinish(conn);
	return 0;
}
