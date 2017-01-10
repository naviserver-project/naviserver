#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://www.mozilla.org/.
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
# configure.ac --
#
#    NaviServer configure script.
#

AC_PREREQ([2.68])

AC_INIT([NaviServer],[4.99.15],[naviserver-devel@lists.sourceforge.net])
AC_CONFIG_MACRO_DIR([m4])

#AM_INIT_AUTOMAKE([1.9 -Wall -Werror foreign])
AC_CONFIG_SRCDIR([include/ns.h])
AC_CONFIG_HEADERS([include/nsconfig.h])

#
# Version info for include/nsversion.h
#
# Don't forget to update the naviserver.rdf DOAP before making a release.
#

AC_SUBST([NS_MAJOR_VERSION],  [4])
AC_SUBST([NS_MINOR_VERSION],  [99])
AC_SUBST([NS_RELEASE_SERIAL], [15])
AC_SUBST([NS_RELEASE_LEVEL],  [NS_FINAL_RELEASE]) # ALPHA/BETA/FINAL

AC_SUBST([NS_VERSION],     [\"${NS_MAJOR_VERSION}.${NS_MINOR_VERSION}\"])
AC_SUBST([NS_PATCH_LEVEL], [\"$PACKAGE_VERSION\"])
#AC_SUBST([NS_PATCH_LEVEL], [\"${NS_MAJOR_VERSION}.${NS_MINOR_VERSION}.${NS_RELEASE_SERIAL}\"])

AC_SUBST([ISO_BUILD_DATE], [`date +%Y-%m-%d`])



#
# Include and verify Tcl config located by --with-tcl=
#

TEA_INIT([3.9])
TEA_PATH_TCLCONFIG
TEA_LOAD_TCLCONFIG
TEA_SETUP_COMPILER
TEA_ENABLE_THREADS
if test "${TCL_THREADS}" != "1"; then
    AC_MSG_ERROR([must build tcl with --enable-threads])
fi
TEA_PUBLIC_TCL_HEADERS
TEA_ENABLE_SHARED
TEA_CONFIG_CFLAGS
TEA_ENABLE_SYMBOLS
TEA_TIME_HANDLER

#
# Extract Tcl library path and library name.
#

TCL_LIB_PATH=`dirname $TCL_STUB_LIB_PATH`
LDFLAGS="$LDFLAGS -L$TCL_LIB_PATH"
AC_SUBST([TCL_LIB_PATH])
AC_SUBST([TCL_LIB], [tcl$TCL_VERSION$TCL_DBGX])
AC_SUBST([TCL_EXEC_PREFIX])

AC_SUBST([DISTCHECK_CONFIGURE_FLAGS], [--with-tcl="$TCL_BIN_DIR"])


#
# Modify SHLIB_LD from tclConfig.sh as we specifically
# name shared library _init functions.
#

test "x$prefix" = "xNONE" && prefix="/usr/local/ns"
system=`uname`
case "$system" in
  Darwin)
    LDLIB="cc -dynamiclib -install_name \$(INSTLIB)/\$(LIBFILE)"
    LDSO="cc -bundle"
    CCRFLAG=
    LDRFLAG=
    CCRPATHS=
    LDRPATHS=
  ;;

  MINGW*)
    LDLIB="$SHLIB_LD"
    LDSO="$SHLIB_LD"
  ;;

  *)
    LDLIB="$SHLIB_LD"
    case "$LDLIB" in
      *gcc*)
        LDLIB="$LDLIB -nostartfiles"
      ;;
    esac
    CCRPATH="\$(CCRFLAG):\$(INSTLIB)"
    LDRPATH="\$(LDRFLAG):\$(INSTLIB)"
    if test "$TCL_EXEC_PREFIX" != "$prefix"; then
      CCRPATH="$CCRPATH \$(CCRFLAG):\$(TCL_EXEC_PREFIX)/lib"
      LDRPATH="$LDRPATH \$(LDRFLAG):\$(TCL_EXEC_PREFIX)/lib"
    fi
    if test -d /usr/lib/lwp; then
      CCRPATH="$CCRPATH \$(CCRFLAG):/usr/lib/lwp"
      LDRPATH="$LDRPATH \$(LDRFLAG):/usr/lib/lwp"
    fi
    LDSO="\$(LDLIB)"
    CCRPATHS="\$(CCRPATH)"
    LDRPATHS="\$(LDRPATH)"
    CCRFLAG=$CC_SEARCH_FLAGS
    LDRFLAG=$LD_SEARCH_FLAGS
    if test "$CCRFLAG" = "" ; then
      CCRPATH=
    fi
    if test "$LDRFLAG" = "" ; then
      LDRPATH=
    fi
  ;;
esac

SRCDIR=`cd $srcdir; pwd`
NAVISERVER=$prefix
AC_SUBST([SRCDIR])
AC_SUBST([LDLIB])
AC_SUBST([LDSO])
AC_SUBST([CCRFLAG])
AC_SUBST([LDRFLAG])
AC_SUBST([CCRPATH])
AC_SUBST([LDRPATH])
AC_SUBST([SHLIB_SUFFIX])
AC_SUBST([TCL_EXEC_PREFIX])
AC_SUBST([NAVISERVER])


#
# Additional checks.
#

AC_CHECK_HEADERS([inttypes.h uio.h sys/uio.h stdint.h netinet/tcp.h sys/sendfile.h])
AC_CHECK_HEADER([mach-o/dyld.h], AC_DEFINE([USE_DYLD], [1], [Define to 1 if the <mach-o/dyld.h> header should be used.]),)
AC_CHECK_HEADER([dl.h], AC_DEFINE([USE_DLSHL], [1], [Define to 1 if the <dl.h> header should be used.]),)

AC_CHECK_FUNCS([timegm fork1 drand48 random _NSGetEnviron unsetenv inet_ntop inet_pton])

AC_SYS_LARGEFILE

AC_TYPE_INT8_T
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT64_T

AC_TYPE_UINT8_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T

AC_TYPE_INTMAX_T
AC_TYPE_UINTMAX_T

AC_TYPE_INTPTR_T
AC_TYPE_UINTPTR_T

AX_HAVE_CMMSG
AX_HAVE_MTSAFE_DNS
AX_HAVE_GETTID
AX_HAVE_TCP_FASTOPEN
AX_CHECK_ZLIB
AX_CHECK_OPENSSL
AX_HAVE_GETPWNAM_R
AX_HAVE_GETPWUID_R
AX_HAVE_GETGRNAM_R
AX_HAVE_GETGRGID_R
AX_HAVE_ARC4RANDOM
AX_HAVE_CRYPT_R

AC_MSG_CHECKING([need for dup high])
AC_RUN_IFELSE([AC_LANG_SOURCE([[
  #include <stdio.h>
  int main(int argc, char **argv) {
    return (sizeof(stdout->_file) == 1) ? 0 : 1;
  }
]])],[AC_DEFINE([USE_DUPHIGH], [1], need for dup high) as_ok=yes],[as_ok=no],[as_ok=no])
AC_MSG_RESULT([$as_ok])

AC_CHECK_TYPE([nfds_t],
	[AC_DEFINE([NS_POLL_NFDS_TYPE],[nfds_t],[type of second ns_poll() argument])],
	[AC_DEFINE([NS_POLL_NFDS_TYPE],[unsigned int],[type of second ns_poll() argument])],
	[#include <poll.h>])

#
# Tcl_GetMemoryInfo() is exported depending on a complex set of compilation
# flags. Physically check to see if it's actually available.
#

AC_CHECK_LIB([$TCL_LIB], [Tcl_GetMemoryInfo],
             AC_DEFINE([HAVE_TCL_GETMEMORYINFO], [1],
                       [Define to 1 if Tcl exports the Tcl_GetMemoryInfo function.]))

#
# Darwin's port of poll() is broken per default as it may return
# socket as writable although it ain't. However, setting SO_SNDLOWAT
# seems to help for Darwin. For platforms without working poll(), we
# still provide our own alternative implementation which is known to
# be working (but does not report POLLHUP).

AC_CHECK_FUNCS([poll])
AC_CHECK_FUNCS([gettimeofday])

#
# Detemine the maximum value of time_t depending on the size of the
# time_t structure.
#

AC_CHECK_SIZEOF([time_t]) 
if test "x$ac_cv_sizeof_time_t" = "x4"; then 
  AC_MSG_RESULT([time_t is 32 bit]) 
  AC_DEFINE([TIME_T_MAX], INT_MAX, [Maximum value for time_t]) 
elif test "x$ac_cv_sizeof_time_t" = "x8"; then 
  AC_MSG_RESULT([time_t is 64 bit]) 
  AC_DEFINE([TIME_T_MAX], LONG_MAX, [Maximum value for time_t]) 
else 
  AC_MSG_ERROR([can not figure type of time_t]) 
fi 


AC_ARG_ENABLE([ipv6],
	AS_HELP_STRING([--disable-ipv6],
		[NaviServer with IPv6 support (default: enabled)]),
        [enable_ipv6=$enableval], [enable_ipv6=yes])
	
if test "$enable_ipv6" = yes; then
   AC_DEFINE([HAVE_IPV6], [1], [Are we building NaviServer with IPv6 support?])
fi
#
# Determine detailed revision info (package tag) either
# - from mercurial (when there is a .hg directory), or
# - assume an installation from a release tar ball
#

AC_MSG_CHECKING([package tag number from source control])
if test -d ".hg" ; then
   ns_package_tag=`hg id -ibt`
else
   ns_package_tag="tar-$PACKAGE_VERSION"
fi
AC_DEFINE_UNQUOTED([PACKAGE_TAG], "$ns_package_tag", 
       	     [Revision number form souce code management system, or a constant from tar distribution])


#
# Normally, we'd link the resulting server image
# dynamically. In some cases it is however more
# appropriate to build it as a monolithic image.
#

AC_MSG_CHECKING([how to build server image])
AC_ARG_ENABLE([shared],
  [  --enable-shared         build and link with shared libraries [--enable-shared]],
   shared_ok=$enable_shared, shared_ok=yes)
if test "$shared_ok" = "yes" ; then
  if test "${TCL_SHARED_BUILD}" != "1" ; then
    AC_MSG_ERROR([must build tcl with --enable-shared])
  fi
  STATIC_BUILD=0
  AC_MSG_RESULT([shared])
else
  if test "${TCL_SHARED_BUILD}" != "0" ; then
    AC_MSG_ERROR([must build tcl with --disable-shared])
  fi
  STATIC_BUILD=1
  AC_MSG_RESULT([static])
fi
AC_SUBST([STATIC_BUILD])

AC_CACHE_CHECK([for shared libgcc], ac_cv_shared_libgcc, [
  ac_save_LIBS="$LIBS"
  LIBS="-lgcc_s $LIBS"
  AC_LINK_IFELSE([AC_LANG_PROGRAM([[]], [[return 0]])],[ac_cv_shared_libgcc=yes],[ac_cv_shared_libgcc=no])
  LIBS="$ac_save_LIBS"
])
if test "$ac_cv_shared_libgcc" = "yes" ; then
  LIBS="-lgcc_s $LIBS"
fi


#
# Darwin version of getnameinfo will not work correctly
# if we do not explicitly set sin_len element of the
# struct sockaddr_in to the total size of the structure.
# Some OS variants however do not have the sin_len member
# so test if this is the case.
#

AC_CHECK_MEMBERS([struct sockaddr_in.sin_len],,,[
#include <sys/types.h>
#include <netinet/in.h>])



AC_CHECK_LIB([crypt], [crypt],
  [
    AC_DEFINE([HAVE_CRYPT], [],
              [Define to 1 if the native crypt library should be used])
    CRYPT_LIBS="-lcrypt"
  ],
  CRYPT_LIBS=""
)
AC_SUBST([CRYPT_LIBS])



AC_CONFIG_FILES([
include/nsversion.h
include/Makefile.global
include/Makefile.module
sample-config.tcl
naviserver.rdf
version_include.man
])
AC_OUTPUT
