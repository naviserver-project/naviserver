#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The Initial Developer of the Original Code and related documentation
# is America Online, Inc. Portions created by AOL are Copyright (C) 1999
# America Online, Inc. All Rights Reserved.
#
AC_DEFUN([AX_HAVE_GETTID], [
AC_MSG_CHECKING([for gettid system call])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <unistd.h>
        #include <sys/syscall.h>
    ]], [[
        int main(void) { return syscall(SYS_gettid); }
    ]])], [
        AC_DEFINE([HAVE_GETTID],1,[Define to 1 when gettid system call is available.])
        AC_MSG_RESULT([yes])
    ],[
        AC_MSG_RESULT([no])
    ])
]) # AX_HAVE_GETTID

AC_DEFUN([AX_HAVE_TCP_FASTOPEN], [
AC_MSG_CHECKING([for TCP_FASTOPEN support])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <linux/tcp.h>
    ]], [[
        int main(void)  { return TCP_FASTOPEN != 0; }
    ]])], [
        AC_DEFINE([HAVE_TCP_FASTOPEN],1,[Define to 1 when TCP_FASTOPEN is available.])
        AC_MSG_RESULT([yes])
    ],[
        AC_MSG_RESULT([no])
    ])
]) # AX_HAVE_TCP_FASTOPEN
