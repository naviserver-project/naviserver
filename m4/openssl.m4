#------------------------------------------------------------------------
# AC_CHECK_OPENSSL --
#
#       Set the openssl compile flags, possibly using a special directory
#       or pkg-config (when --with-openssl is specified without parameters)
#
# Arguments:
#       optional directory, such as e.g. --with-openssl=/opt/local
#
# Results:
#
#       Adds the following arguments to configure:
#               --with-openssl=[dir]
#
#       Defines the following vars:
#               OPENSSL_INCLUDES   Full path to the directory containing
#                                  the openssl/evp.h file if a openssl
#                                  directory was specified.
#               OPENSSL_LIBS       Linker line for openssl.
#------------------------------------------------------------------------

AC_DEFUN([AX_CHECK_OPENSSL], [
AC_MSG_CHECKING([for OpenSSL libraries])
AC_ARG_WITH([openssl],
  AS_HELP_STRING(--with-openssl=DIR,Build and link with OpenSSL),
  [
    ac_openssl=$withval
    if test "${ac_openssl}" != "no" ; then
      ac_openssl=yes
      if test -d "$withval" ; then
        echo "Trying to use directory $withval/include and -L$withval/lib"
        OPENSSL_INCLUDES="-I$withval/include"
        OPENSSL_LIBS="-L$withval/lib -lssl -lcrypto"
      fi
    fi
  ],
  [
    ac_openssl="yes"
    OPENSSL_INCLUDES=""
    OPENSSL_LIBS="-lssl -lcrypto"
  ])


# AC_MSG_RESULT([$ac_openssl])

if test "${ac_openssl}" = "yes" ; then

  dnl OpenSSL is configured
  dnl Was a path being provided?
  if test -z "$OPENSSL_INCLUDES"; then
     dnl No path provided, check PKG_CONFIG
     if test -z "$PKG_CONFIG"; then
     	AC_PATH_PROG(PKG_CONFIG, pkg-config, no)
     fi
     if test -x "$PKG_CONFIG" && $PKG_CONFIG --exists openssl; then
     	echo "OpenSSL is configured via $PKG_CONFIG"
     	OPENSSL_LIBS=`$PKG_CONFIG --libs openssl`
     	OPENSSL_INCLUDES=`$PKG_CONFIG --cflags-only-I openssl`
     fi
  fi

  save_CPPFLAGS="$CPPFLAGS"
  save_LIBS="$LIBS"
  CPPFLAGS="$OPENSSL_INCLUDES $CPPFLAGS"
  LIBS="$LIBS $OPENSSL_LIBS"

  AC_CHECK_HEADERS([openssl/evp.h])
  FOUND_SSL_LIB="no"
  AC_CHECK_LIB(ssl, OPENSSL_init_ssl, [FOUND_SSL_LIB="yes"])
  AC_CHECK_LIB(ssl, SSL_library_init, [FOUND_SSL_LIB="yes"])
  AC_CHECK_LIB([crypto], [PEM_read_bio_DHparams])
  
  dnl echo "OpenSSL headers found:    $ac_cv_header_openssl_evp_h"
  dnl echo "OpenSSL lib ssl found:    $ac_cv_lib_ssl_SSL_library_init"
  dnl echo "OpenSSL lib crypto found: $ac_cv_lib_crypto_PEM_read_bio_DHparams"

  if test "${ac_cv_header_openssl_evp_h}" != "yes" -o "${FOUND_SSL_LIB}" != "yes" -o "${ac_cv_lib_crypto_PEM_read_bio_DHparams}" != "yes"; then
    AC_MSG_ERROR([OpenSSL support requested but not available])
  fi

  CPPFLAGS="$save_CPPFLAGS"
  LIBS="$save_LIBS"
fi

AC_SUBST([OPENSSL_INCLUDES])
AC_SUBST([OPENSSL_LIBS])

])
