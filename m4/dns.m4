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
# $Header$
#

dnl
dnl Check to see what variant of gethostbyname_r() we have.  Defines
dnl HAVE_GETHOSTBYNAME_R_{6, 5, 3} depending on what variant is found.
dnl
dnl Based on David Arnold's example from the comp.programming.threads
dnl FAQ Q213.
dnl

AC_DEFUN([AX_HAVE_GETHOSTBYNAME_R],
[saved_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS -lnsl"
AC_CHECK_FUNC(gethostbyname_r, [
  AC_MSG_CHECKING([for gethostbyname_r with 6 args])
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <netdb.h>
  ]], [[
    char *name;
    struct hostent *he, *res;
    char buffer[2048];
    int buflen = 2048;
    int h_errnop;

    (void) gethostbyname_r(name, he, buffer, buflen, &res, &h_errnop);
  ]])],[
    AC_DEFINE(HAVE_GETHOSTBYNAME_R,1,[Define to 1 if gethostbyname_r is available.])
    AC_DEFINE(HAVE_GETHOSTBYNAME_R_6,1,[Define to 1 if gethostbyname_r takes 6 args.])
    AC_MSG_RESULT(yes)
  ],[
    AC_MSG_RESULT(no)
    AC_MSG_CHECKING([for gethostbyname_r with 5 args])

    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
      #include <netdb.h>
    ]], [[
      char *name;
      struct hostent *he;
      char buffer[2048];
      int buflen = 2048;
      int h_errnop;

      (void) gethostbyname_r(name, he, buffer, buflen, &h_errnop);
    ]])],[
      AC_DEFINE(HAVE_GETHOSTBYNAME_R,1,[Define to 1 if gethostbyname_r is available.])
      AC_DEFINE(HAVE_GETHOSTBYNAME_R_5,1,[Define to 1 if gethostbyname_r takes 5 args.])
      AC_MSG_RESULT(yes)
    ],[
      AC_MSG_RESULT(no)
      AC_MSG_CHECKING([for gethostbyname_r with 3 args])

      AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <netdb.h>
      ]], [[
        char *name;
        struct hostent *he;
        struct hostent_data data;

        (void) gethostbyname_r(name, he, &data);
      ]])],[
        AC_DEFINE(HAVE_GETHOSTBYNAME_R,1,[Define to 1 if gethostbyname_r is available.])
        AC_DEFINE(HAVE_GETHOSTBYNAME_R_3,1,[Define to 1 if gethostbyname_r takes 3 args.])
        AC_MSG_RESULT(yes)
      ],[
        AC_MSG_RESULT(no)
      ])
    ])
  ])
])
CFLAGS="$saved_CFLAGS"])



AC_DEFUN([AX_HAVE_GETHOSTBYADDR_R],
[saved_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS -lnsl"
AC_CHECK_FUNC(gethostbyaddr_r, [
  AC_MSG_CHECKING([for gethostbyaddr_r with 7 args])
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
    #include <netdb.h>
  ]], [[
    char *addr;
    int length;
    int type;
    struct hostent *result;
    char buffer[2048];
    int buflen = 2048;
    int h_errnop;

    (void) gethostbyaddr_r(addr, length, type, result, buffer, buflen, &h_errnop);
  ]])],[
    AC_DEFINE(HAVE_GETHOSTBYADDR_R,1,[Define to 1 if gethostbyaddr_r is available.])
    AC_DEFINE(HAVE_GETHOSTBYADDR_R_7,1,[Define to 1 if gethostbyaddr_r takes 7 args.])
    AC_MSG_RESULT(yes)
  ],[
    AC_MSG_RESULT(no)
  ])
])
CFLAGS="$saved_CFLAGS"])



AC_DEFUN([AX_HAVE_MTSAFE_DNS],
[
  AC_CHECK_LIB(socket, getaddrinfo)
  AC_CHECK_LIB(socket, getnameinfo)
  AC_CHECK_FUNCS(getaddrinfo getnameinfo)

  if test "${ac_cv_func_getaddrinfo}" = "yes" \
       -a "${ac_cv_func_getnameinfo}" = "yes" ; then \
    if test "`uname -s`" = "Darwin"; then \
        AC_MSG_CHECKING(if we have working getaddrinfo)
	AC_RUN_IFELSE([AC_LANG_SOURCE([[#include <mach-o/dyld.h>
#include <stdlib.h>
int main() { if (NSVersionOfRunTimeLibrary("System") >= (60 << 16)) {exit(0);} else {exit(1);}}]])],[have_mtsafe_dns=yes],[have_mtsafe_dns=no],[])
    else
        have_mtsafe_dns=yes
    fi
  fi
  if test "${have_mtsafe_dns}" != "yes" \
       -a "`uname -s`" != "Darwin"; then
    AX_HAVE_GETHOSTBYNAME_R
    AX_HAVE_GETHOSTBYADDR_R
    if test "${ac_cv_func_gethostbyname_r}" = "yes" \
         -a "${ac_cv_func_gethostbyaddr_r}" = "yes" ; then
      have_mtsafe_dns=yes
    fi
  fi
  if test "${have_mtsafe_dns}" != "yes" ; then
    AC_MSG_WARN([DNS queries use MT-unsafe calls which could result in server instability])
  else
    AC_DEFINE(HAVE_MTSAFE_DNS, 1, [Define to 1 if DNS calls are MT-safe])
  fi
])
