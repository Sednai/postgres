#----------------------------------------------------------------------------
#
# Postgres-XC GTM client test makefile
#
# Copyright(c) 2010-2012 Postgres-XC Development Group
#
# src/gtm/client/test/Makefile
#
#-----------------------------------------------------------------------------
top_builddir=../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(libpq_srcdir) $(CPPFLAGS)

override CFLAGS += -I$(top_builddir)/src/include

OBJS=test_seq.o test_txn.o test_snap.o test_txnperf.o test_snapperf.o
#LIBS = $(PTHREAD_LIBS) $(top_builddir)/src/gtm/client/libgtmclient.a $(top_builddir)/src/gtm/common/libgtmcommon.a  $(top_builddir)/src/gtm/libpq/libgtmpq.a
LIBS += $(PTHREAD_LIBS)

include $(top_srcdir)/src/backend/common.mk

CFLAGS += -g -O0

all:test_txn test_seq test_snap test_txnperf test_snapperf

test_txn: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(LIBS) test_txn.o $(top_builddir)/src/gtm/client/libgtmclient.a $(top_builddir)/src/gtm/common/libgtmcommon.a $(top_builddir)/src/gtm/libpq/libgtmpq.a

#test_txn:test_txn.o $(LIBS)

test_seq:test_seq.o $(LIBS)

test_snap:test_snap.o $(LIBS)

test_txnperf:test_txnperf.o $(LIBS)

test_snapperf:test_snapperf.o $(LIBS)

clean:
	rm -f $(OBJS)
	rm -f test_txn test_seq test_snap test_txnperf test_snapperf

distclean: clean

maintainer-clean: distclean
