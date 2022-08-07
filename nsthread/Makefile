#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
#
#

LIBNM	= nsthread
DLL	= $(LIBNM)
HDRS	= thread.h
PGM	= nsthreadtest
PGMOBJS	= nsthreadtest.o
LIBOBJS = error.o master.o memory.o mutex.o cslock.o \
	  rwlock.o reentrant.o sema.o thread.o tls.o time.o \
	  pthread.o fork.o signal.o winthread.o
PGMLIBS = -lpthread 

# Note that when building on Windows, you need the various centralized
# LIB, INCLUDE, TCLPATH, etc. settings found in naviserver/Makefile.win32,
# so just running this nsthread/Makefile directly will not work.
# --atp@piskorski.com, 2014/09/23 13:40 EDT

include ../include/Makefile.build
