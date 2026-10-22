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
LIBOBJS = atomic.o error.o master.o memory.o mutex.o cslock.o \
	  rwlock.o reentrant.o sema.o thread.o tls.o time.o \
	  pthread.o fork.o signal.o winthread.o
PGMLIBS = -lpthread 

# Note that when building on Windows, you need the various centralized
# LIB, INCLUDE, TCLPATH, etc. settings found in naviserver/Makefile.win32,
# so just running this nsthread/Makefile directly will not work.
# --atp@piskorski.com, 2014/09/23 13:40 EDT

include ../include/Makefile.build

# Atomic regression targets use GNU make and a Unix shell. Keep this section
# out of nmake's production-library build, like Makefile.build's dispatch.
# \
!ifndef 0 # \
!else

.PHONY: check clean-atomic
clean: clean-atomic
check: atomictest$(EXEEXT) atomic-fallback-test$(EXEEXT)
	LD_LIBRARY_PATH="$(CURDIR):$${LD_LIBRARY_PATH}" DYLD_LIBRARY_PATH="$(CURDIR):$${DYLD_LIBRARY_PATH}" ./atomictest$(EXEEXT)
	LD_LIBRARY_PATH="$(CURDIR):$${LD_LIBRARY_PATH}" DYLD_LIBRARY_PATH="$(CURDIR):$${DYLD_LIBRARY_PATH}" ./atomic-fallback-test$(EXEEXT)

atomictest$(EXEEXT): atomictest.o $(LIBFILE)
	$(CC) $(LDFLAGS) -o $@ atomic-fallback-test.o $(PGMLIBS) $(CCLIBS) $(CCRPATH)

atomic-fallback-test$(EXEEXT): atomic-fallback-test.o $(LIBFILE)
	$(CC) $(LDFLAGS) -o $@ atomic-fallback-test.o $(PGMLIBS) $(CCLIBS)

atomictest.o: atomictest.c ../include/nsthread.h
atomic-fallback-test.o: atomic-fallback-test.c atomictest.c atomic.c thread.h ../include/nsthread.h

clean-atomic:
	$(RM) atomictest$(EXEEXT) atomic-fallback-test$(EXEEXT) atomictest.o atomic-fallback-test.o
# \
!endif
