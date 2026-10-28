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

MODNAME = nsdb
MOD     = nsdb.so
MODOBJS = nsdb.o
LIBNM   = nsdb
LIBOBJS = dbinit.o dbdrv.o dbtcl.o dbutil.o batch.o
LIBHDRS = nsdb.h batch.h
HDRS    = db.h batch.h ../nsd/nsatoms.h ../nsd/nsd.h
TCL     = util.tcl

include ../include/Makefile.build

batch.o: ../nsd/nsatoms.h ../nsd/nsd.h
