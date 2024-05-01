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

dnl
dnl Check to see if msghdr structure can support BSD4.4 style message passing.
dnl
dnl Defines HAVE_CMMSG.
dnl

AC_DEFUN([AX_HAVE_CMMSG],
[AC_CACHE_CHECK([for cmmsg],
                [ax_cv_have_cmmsg],
                [_AX_HAVE_CMMSG([ax_cv_have_cmmsg=yes], [ax_cv_have_cmmsg=no])] )
if test x$ax_cv_have_cmmsg = xyes; then
  AC_DEFINE([HAVE_CMMSG], 1,
              [Define if you have support for BSD4.4 style msg passing.])
fi]) # AX_HAVE_CMMSG


# _AX_HAVE_CMMSG(HAVE, DO-NOT-HAVE)
#----------------------------------
AC_DEFUN([_AX_HAVE_CMMSG],
[AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <sys/types.h>
#include <sys/socket.h>
]], [[struct msghdr msg; msg.msg_control = 0;]])], [$1],[$2])])


AC_DEFUN([AX_CHECK_SIZEOF_MSG_IOVLEN], [
  AC_MSG_CHECKING([if msg_iovlen in msghdr is of type size_t])
  AC_RUN_IFELSE(
    [AC_LANG_PROGRAM([[#include <sys/types.h>
                       #include <sys/socket.h>]],
                     [[struct msghdr msg;
                       return (sizeof(msg.msg_iovlen) != sizeof(size_t));]])],
    [AC_MSG_RESULT([yes])
     AC_DEFINE([NS_MSG_IOVLEN_IS_SIZE_T], [1], [Define to 1 if msg_iovlen is of type size_t])],
    [AC_MSG_RESULT([no])
     AC_DEFINE([NS_MSG_IOVLEN_IS_SIZE_T], [0], [Could not determine if msghdr.msg_iovlen is of type size_t.])])
])

