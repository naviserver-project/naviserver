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
dnl Check to see whether we have the arc4random generator
dnl available.
dnl

AC_DEFUN([AX_HAVE_ARC4RANDOM],
  [AC_CHECK_FUNC([arc4random], [
    AC_DEFINE([HAVE_ARC4RANDOM],[1],[Define to 1 if arc4random is available.])
  ])]
)

dnl AC_DEFUN([AX_HAVE_CRYPT_R],
dnl    [AC_CHECK_FUNC([crypt_r], [
dnl     AC_DEFINE([HAVE_CRYPT_R],[1],[Define to 1 if crypt_r is available.])
dnl   ])]
dnl )

AC_DEFUN([AX_HAVE_CRYPT_R], [
AC_MSG_CHECKING([for crypt_r library function])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <crypt.h>
    ]], [[
        int main(void) { struct crypt_data d; char *r = crypt_r("", "", &d);return 0; }
    ]])], [
        AC_DEFINE([HAVE_CRYPT_R],1,[Define to 1 when crypt_r library function is available.])
        AC_MSG_RESULT([yes])
    ],[
        AC_MSG_RESULT([no])
    ])
]) # AX_HAVE_CRYPT_R

dnl
dnl Check to see whether we have the memmem available.
dnl

AC_DEFUN([AX_HAVE_MEMMEM],
  [AC_CHECK_FUNC([memmem], [
    AC_DEFINE([HAVE_MEMMEM],[1],[Define to 1 if memmem is available.])
  ])]
)
