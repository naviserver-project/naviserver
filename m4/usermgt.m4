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
dnl Check to see whether we have the get*_r function for user and group
dnl management available.
dnl

AC_DEFUN([AX_HAVE_GETPWNAM_R],
  [AC_CHECK_FUNC([getpwnam_r], [
    AC_DEFINE([HAVE_GETPWNAM_R],[1],[Define to 1 if getpwnam_r is available.])
  ])]
)

AC_DEFUN([AX_HAVE_GETPWUID_R],
  [AC_CHECK_FUNC([getpwuid_r], [
    AC_DEFINE([HAVE_GETPWUID_R],[1],[Define to 1 if getpwuid_r is available.])
  ])]
)

AC_DEFUN([AX_HAVE_GETGRNAM_R],
  [AC_CHECK_FUNC([getgrnam_r], [
    AC_DEFINE([HAVE_GETGRNAM_R],[1],[Define to 1 if getgrnam_r is available.])
  ])]
)

AC_DEFUN([AX_HAVE_GETGRGID_R],
  [AC_CHECK_FUNC([getgrgid_r], [
    AC_DEFINE([HAVE_GETGRGID_R],[1],[Define to 1 if getgrgid_r is available.])
  ])]
)
