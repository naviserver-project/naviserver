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
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/uio.h>
    ]],[[
    int fd, s, flags;
    off_t offset, sbytes;
    size_t nbytes;
    struct sf_hdtr hdtr;
    (void) sendfile(fd, s, offset, nbytes, &hdtr, &sbytes, flags);
    ]])], [bsd_cv_sendfile=yes], [bsd_cv_sendfile=no])])
    bsd_ok=$bsd_cv_sendfile
    if test "$bsd_ok" = yes; then
        AC_DEFINE(HAVE_BSD_SENDFILE, 1, [Define to 1 for BSD-type sendfile])
    fi
])])                                                                            

AC_DEFUN([AX_HAVE_LINUX_SENDFILE], [AC_CHECK_FUNC(sendfile, [
    AC_CACHE_CHECK([for Linux-compatible sendfile], linux_cv_sendfile, [
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <sys/sendfile.h>
    ]],[[
    int out, in;
    off_t offset;
    size_t count;
    (void) sendfile(out, in, &offset, count);
    ]])], [linux_cv_sendfile=yes], [linux_cv_sendfile=no])])
    linux_ok=$linux_cv_sendfile
    if test "$linux_ok" = yes; then
        AC_DEFINE(HAVE_LINUX_SENDFILE, 1, [Define to 1 for Linux-type sendfile])
    fi
])])

