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

