#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://aolserver.com/.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# The Original Code is AOLserver Code and related documentation
# distributed by AOL.
#
# The Initial Developer of the Original Code is America Online,
# Inc. Portions created by AOL are Copyright (C) 1999 America Online,
# Inc. All Rights Reserved.
#
# Alternatively, the contents of this file may be used under the terms
# of the GNU General Public License (the "GPL"), in which case the
# provisions of GPL are applicable instead of those above.  If you wish
# to allow use of your version of this file only under the terms of the
# GPL and not to allow others to use your version of this file under the
# License, indicate your decision by deleting the provisions above and
# replace them with the notice and other provisions required by the GPL.
# If you do not delete the provisions above, a recipient may use your
# version of this file under either the License or the GPL.
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
	  crypt.o dns.o driver.o dstring.o encoding.o event.o exec.o \
	  fastpath.o fd.o filter.o form.o hmac_sha2.c httptime.o index.o info.o \
	  init.o limits.o lisp.o listen.o log.o mimetypes.o modload.o nsconf.o \
	  nsmain.o nsthread.o op.o pathname.o pidfile.o proc.o progress.o queue.o \
	  quotehtml.o random.o range.o request.o return.o returnresp.o rollfile.o \
	  sha2.o sched.o server.o set.o sls.o sock.o sockcallback.o sockfile.o str.o \
	  task.o tclcache.o tclcallbacks.o tclcmds.o tclconf.o tclenv.o tclfile.o \
	  tclhttp.o tclimg.o tclinit.o tcljob.o tclmisc.o tclobj.o tclobjv.o \
	  tclrequest.o tclresp.o tclsched.o tclset.o tclsock.o sockaddr.o \
	  tclthread.o tcltime.o tclvar.o tclxkeylist.o stamp.o \
	  url.o url2file.o urlencode.o urlopen.o urlspace.o uuencode.o \
	  unix.o watchdog.o nswin32.o

include ../include/Makefile.build

install-init:
	$(INSTALL_DATA) init.tcl $(DESTDIR)$(INSTBIN)
