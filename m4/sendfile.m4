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

dnl
dnl Check to see whether we have the sendfile a'la Linux/Solaris
dnl or FreeBSD/Darwin (or not at all).
dnl
dnl Might define the following vars:
dnl
dnl      HAVE_BSD_SENDFILE
dnl      HAVE_LINUX_SENDFILE
dnl

AC_DEFUN([AX_HAVE_BSD_SENDFILE], [AC_CHECK_FUNC(sendfile, [
    AC_CACHE_CHECK([for BSD-compatible sendfile], bsd_cv_sendfile, [
    AC_TRY_COMPILE([
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/uio.h>
    ], [
    int fd, s, flags;
    off_t offset, sbytes;
    size_t nbytes;
    struct sf_hdtr hdtr;
    (void) sendfile(fd, s, offset, nbytes, &hdtr, &sbytes, flags);
    ], bsd_cv_sendfile=yes, bsd_cv_sendfile=no)])
    bsd_ok=$bsd_cv_sendfile
    if test "$bsd_ok" = yes; then
        AC_DEFINE(HAVE_BSD_SENDFILE, 1, [Define to 1 for BSD-type sendfile])
    fi
])])                                                                            

AC_DEFUN([AX_HAVE_LINUX_SENDFILE], [AC_CHECK_FUNC(sendfile, [
    AC_CACHE_CHECK([for Linux-compatible sendfile], linux_cv_sendfile, [
    AC_TRY_COMPILE([
    #include <sys/sendfile.h>
    ], [
    int out, in;
    off_t offset;
    size_t count;
    (void) sendfile(out, in, &offset, count);
    ], linux_cv_sendfile=yes, linux_cv_sendfile=no)])
    linux_ok=$linux_cv_sendfile
    if test "$linux_ok" = yes; then
        AC_DEFINE(HAVE_LINUX_SENDFILE, 1, [Define to 1 for Linux-type sendfile])
    fi
])])

