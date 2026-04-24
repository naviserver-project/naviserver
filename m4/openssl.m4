#------------------------------------------------------------------------
# AX_CHECK_OPENSSL --
#
#       Set the OpenSSL compile flags, possibly using a special directory
#       or pkg-config (when --with-openssl is specified without parameters)
#
# Arguments:
#       optional directory:
#           --with-openssl=DIR
#
#       optional include/lib directory pair:
#           --with-openssl=INCLUDEDIR,LIBDIR
#
# Results:
#
#       Adds the following argument to configure:
#               --with-openssl[=DIR|INCLUDEDIR,LIBDIR]
#
#       Defines the following vars:
#               OPENSSL_INCLUDES   Full path to the directory containing
#                                  the openssl/evp.h file if explicitly
#                                  specified.
#               OPENSSL_LIBS       Linker line for OpenSSL.
#------------------------------------------------------------------------

AC_DEFUN([AX_CHECK_OPENSSL], [
AC_MSG_CHECKING([for OpenSSL libraries])

AC_ARG_WITH([openssl],
  AS_HELP_STRING(
    [--with-openssl@<:@=DIR|INCLUDEDIR,LIBDIR@:>@],
    [Build and link with OpenSSL from a prefix DIR, or from explicit include/lib directories]
  ),
  [
    ac_openssl=$withval

    if test "${ac_openssl}" != "no" -a "${ac_openssl}" != "yes" ; then
      ac_openssl=yes
      OPENSSL_INCLUDES=""
      OPENSSL_LIBS=""

      dnl
      dnl Two forms are supported:
      dnl   --with-openssl=DIR
      dnl   --with-openssl=INCLUDEDIR,LIBDIR
      dnl
      AS_CASE([$withval],
        [*,*], [
          openssl_incdir=`echo "$withval" | sed 's/,.*$//'`
          openssl_libdir=`echo "$withval" | sed 's/^.*,//'`

          if test -d "$openssl_incdir" -a -d "$openssl_libdir" ; then
            OPENSSL_INCLUDES="-I$openssl_incdir"

            dnl
            dnl Prefer absolute library pathnames when possible.
            dnl
            if test -r "$openssl_libdir/libssl.dylib" -a -r "$openssl_libdir/libcrypto.dylib" ; then
               OPENSSL_LIBS="$openssl_libdir/libssl.dylib $openssl_libdir/libcrypto.dylib"
            elif test -r "$openssl_libdir/libssl.so" -a -r "$openssl_libdir/libcrypto.so" ; then
               OPENSSL_LIBS="$openssl_libdir/libssl.so $openssl_libdir/libcrypto.so"
            elif test -r "$openssl_libdir/libssl.a" -a -r "$openssl_libdir/libcrypto.a" ; then
               OPENSSL_LIBS="$openssl_libdir/libssl.a $openssl_libdir/libcrypto.a"
            elif test -r "$openssl_libdir/libssl.so" ; then
               OPENSSL_LIBS="-L$openssl_libdir -lssl -lcrypto"
            else
               OPENSSL_LIBS="-L$openssl_libdir -lssl -lcrypto"
            fi
            AC_MSG_NOTICE([Trying to use include directory $openssl_incdir and libraries from $openssl_libdir])
          else
            AC_MSG_ERROR([no such include/lib directory pair: $withval])
          fi
        ],
        [
          if test -d "$withval" ; then
            OPENSSL_INCLUDES="-I$withval/include"

            dnl
            dnl When an explicit OpenSSL prefix is provided, prefer absolute
            dnl library pathnames when possible. This avoids accidental
            dnl resolution of -lssl/-lcrypto from unrelated earlier -L flags.
            dnl
            if test -r "$withval/lib/libssl.dylib" -a -r "$withval/lib/libcrypto.dylib" ; then
               OPENSSL_LIBS="$withval/lib/libssl.dylib $withval/lib/libcrypto.dylib"
            elif test -r "$withval/lib64/libssl.so" -a -r "$withval/lib64/libcrypto.so" ; then
               OPENSSL_LIBS="$withval/lib64/libssl.so $withval/lib64/libcrypto.so"
            elif test -r "$withval/lib/libssl.so" -a -r "$withval/lib/libcrypto.so" ; then
               OPENSSL_LIBS="$withval/lib/libssl.so $withval/lib/libcrypto.so"
            elif test -r "$withval/lib64/libssl.a" -a -r "$withval/lib64/libcrypto.a" ; then
               OPENSSL_LIBS="$withval/lib64/libssl.a $withval/lib64/libcrypto.a"
            elif test -r "$withval/lib/libssl.a" -a -r "$withval/lib/libcrypto.a" ; then
               OPENSSL_LIBS="$withval/lib/libssl.a $withval/lib/libcrypto.a"
            elif test -r "$withval/lib64/libssl.so" ; then
               OPENSSL_LIBS="-L$withval/lib64 -lssl -lcrypto"
            else
               OPENSSL_LIBS="-L$withval/lib -lssl -lcrypto"
            fi
            AC_MSG_NOTICE([Trying to use directory $withval/include and $OPENSSL_LIBS])
          else
            AC_MSG_ERROR([no such directory: $withval])
          fi
        ])
    fi
  ],
  [
    ac_openssl="yes"
    OPENSSL_INCLUDES=""
    OPENSSL_LIBS="-lssl -lcrypto"
  ])

if test "${ac_openssl}" = "yes" ; then

  if test -z "$OPENSSL_INCLUDES"; then
     if test -z "$PKG_CONFIG"; then
        AC_PATH_PROG(PKG_CONFIG, pkg-config, no)
     fi
     if test -x "$PKG_CONFIG" && $PKG_CONFIG --exists openssl; then
        AC_MSG_NOTICE([OpenSSL is configured via $PKG_CONFIG])
        OPENSSL_LIBS=`$PKG_CONFIG --libs openssl`
        OPENSSL_INCLUDES=`$PKG_CONFIG --cflags-only-I openssl`
     fi
  fi

dnl ------------------------------------------------------------
dnl Use provided OpenSSL flags during detection
dnl ------------------------------------------------------------

  CPPFLAGS_saved="${CPPFLAGS}"
  LIBS_saved="${LIBS}"
  CFLAGS_saved="${CFLAGS}"
  LDFLAGS_saved="${LDFLAGS}"

  CPPFLAGS="${OPENSSL_INCLUDES} ${CPPFLAGS}"
  CFLAGS="${OPENSSL_INCLUDES} ${CFLAGS}"
  LIBS="${OPENSSL_LIBS} ${LIBS}"
  LDFLAGS="${LDFLAGS_saved}"

  AC_CHECK_HEADERS([openssl/evp.h])

  FOUND_SSL_LIB="no"
  AC_CHECK_LIB([ssl], [OPENSSL_init_ssl], [FOUND_SSL_LIB="yes"])
  AC_CHECK_LIB([crypto], [OPENSSL_init_crypto])

dnl ------------------------------------------------------------
dnl Final decision
dnl ------------------------------------------------------------
  if test "${ac_cv_header_openssl_evp_h}" != "yes" \
     -o "${FOUND_SSL_LIB}" != "yes" \
     -o "${ac_cv_lib_crypto_OPENSSL_init_crypto}" != "yes"; then
    AC_MSG_ERROR([OpenSSL support requested but not available])
  fi

  CPPFLAGS="${CPPFLAGS_saved}"
  LIBS="${LIBS_saved}"
  CFLAGS="${CFLAGS_saved}"
  LDFLAGS="${LDFLAGS_saved}"
fi

AC_SUBST([OPENSSL_INCLUDES])
AC_SUBST([OPENSSL_LIBS])
])
