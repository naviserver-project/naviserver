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
#

# IMPORTANT: Do NOT use the exact same base file name for both nsd.exe
# an nsd.dll.  If you do, NaviServer runs just fine, but the nsd.pdb
# file for nsd.exe OVERWRITES the nsd.pdb for nsd.dll!  The easiest
# fix is to simply name the DLL libnsd.dll instead.

INSTALL	= install-init
LIBNM	= libnsd
DLL	= $(LIBNM)
PGM	= nsd
PGMOBJS	= main.o
HDRS	= nsd.h

LIBOBJS = adpcmds.o adpeval.o adpparse.o adprequest.o auth.o binder.o \
	  cache.o callbacks.o cls.o compress.o config.o conn.o connio.o \
	  cookies.o connchan.o \
	  crypt.o dlist.o dns.o driver.o dstring.o encoding.o event.o exec.o \
	  fastpath.o fd.o filter.o form.o httptime.o index.o info.o \
	  init.o limits.o lisp.o listen.o log.o mimetypes.o modload.o nsconf.o \
	  nsmain.o nsthread.o op.o pathname.o pidfile.o proc.o progress.o queue.o \
	  quotehtml.o random.o range.o request.o return.o returnresp.o rollfile.o \
	  sched.o server.o set.o sls.o sock.o sockcallback.o sockfile.o str.o \
	  task.o tclcache.o tclcallbacks.o tclcmds.o tclconf.o tclenv.o tclfile.o \
	  tclhttp.o tclimg.o tclinit.o tcljob.o tclmisc.o tclobj.o tclobjv.o \
	  tclrequest.o tclresp.o tclsched.o tclset.o tclsock.o sockaddr.o \
	  tclthread.o tcltime.o tclvar.o tclxkeylist.o tls.o stamp.o \
	  url.o url2file.o urlencode.o urlopen.o urlspace.o uuencode.o \
	  unix.o watchdog.o nswin32.o tclcrypto.o tclparsefieldvalue.o

include ../include/Makefile.build

tls.o: dhparams.h nsopenssl.h

dhparams.h:
	openssl dhparam -C -2 -noout 512 >> dhparams.h
	openssl dhparam -C -2 -noout 1024 >> dhparams.h
	openssl dhparam -C -2 -noout 2048 >> dhparams.h

install-init:
	$(INSTALL_DATA) init.tcl $(DESTDIR)$(INSTBIN)
