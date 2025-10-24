dnl ------------------------------------------------------------------------
dnl AX_CHECK_NGHTTP3 --
dnl
dnl     Discover nghttp3 compile/link flags for the optional QUIC/HTTP/3 driver.
dnl     Honors NGHTTP3_CFLAGS/NGHTTP3_LIBS, supports --with-nghttp3=PREFIX,
dnl     tries pkg-config (libnghttp3 and nghttp3), then probes headers/libs.
dnl
dnl Arguments:
dnl     --with-nghttp3[=PREFIX]
dnl
dnl Results:
dnl     Substitutes:
dnl         NGHTTP3_CFLAGS   Compiler flags (e.g., -I...).
dnl         NGHTTP3_LIBS     Linker flags  (e.g., -L... -lnghttp3).
dnl     Defines:
dnl         HAVE_NGHTTP3     1 if nghttp3 was found.
dnl     Automake conditional:
dnl         HAVE_NGHTTP3
dnl
dnl Notes:
dnl     * If pkg-config can’t find the .pc file, set PKG_CONFIG_PATH.
dnl     * --with-nghttp3=no skips detection and leaves vars empty.
dnl ------------------------------------------------------------------------

AC_DEFUN([AX_CHECK_NGHTTP3], [
  AC_ARG_WITH([nghttp3],
    [AS_HELP_STRING([--with-nghttp3[=PREFIX]],
                    [nghttp3 install prefix (or "no" to disable auto-discovery)])],
    [with_nghttp3="$withval"], [with_nghttp3="yes"])

  have_nghttp3="no"

  dnl Respect explicit env overrides first
  if test "x$NGHTTP3_CFLAGS" != x -o "x$NGHTTP3_LIBS" != x ; then
    have_nghttp3="yes"
  fi

  dnl If a prefix was given, prime hints (works with or without pkg-config)
  if test "x$with_nghttp3" != "xyes" -a "x$with_nghttp3" != "xno"; then
    if test "x$NGHTTP3_CFLAGS" = x ; then
      NGHTTP3_CFLAGS="-I$with_nghttp3/include"
    fi
    if test "x$NGHTTP3_LIBS" = x ; then
      NGHTTP3_LIBS="-L$with_nghttp3/lib -lnghttp3"
    fi
  fi

  dnl Try pkg-config unless user said --with-nghttp3=no or env already set
  if test "x$have_nghttp3" = "xno" -a "x$with_nghttp3" != "xno"; then
    PKG_PROG_PKG_CONFIG
    if test "x$PKG_CONFIG" != "x"; then
      dnl Distros vary: .pc can be libnghttp3 or nghttp3
      PKG_CHECK_MODULES([NGHTTP3], [libnghttp3], [have_nghttp3="yes"], [
        PKG_CHECK_MODULES([NGHTTP3], [nghttp3], [have_nghttp3="yes"], [:])
      ])
    fi
  fi

  dnl Fallback: header+symbol check
  if test "x$have_nghttp3" = "xno" -a "x$with_nghttp3" != "xno"; then
    save_CFLAGS="$CFLAGS"
    save_LIBS="$LIBS"
    CFLAGS="$CFLAGS $NGHTTP3_CFLAGS"
    LIBS="$LIBS $NGHTTP3_LIBS"

    AC_CHECK_HEADERS([nghttp3/nghttp3.h], [
      AC_CHECK_LIB([nghttp3], [nghttp3_conn_client_new],
        [have_nghttp3="yes"; test "x$NGHTTP3_LIBS" = "x" && NGHTTP3_LIBS="-lnghttp3"],
        [:])
    ], [:])

    CFLAGS="$save_CFLAGS"
    LIBS="$save_LIBS"
  fi

  AS_IF([test "x$have_nghttp3" = "xyes"], [
    AC_DEFINE([HAVE_NGHTTP3], [1], [Define if nghttp3 is available])
  ])

  AC_SUBST([NGHTTP3_CFLAGS])
  AC_SUBST([NGHTTP3_LIBS])

  dnl Friendly summary
  if test "x$have_nghttp3" = "xyes"; then
    AC_MSG_RESULT([nghttp3: yes])
    AC_MSG_NOTICE([NGHTTP3_CFLAGS: $NGHTTP3_CFLAGS])
    AC_MSG_NOTICE([NGHTTP3_LIBS:   $NGHTTP3_LIBS])
  else
    AC_MSG_RESULT([nghttp3: no])
  fi

  AM_CONDITIONAL([HAVE_NGHTTP3], [test "x$have_nghttp3" = "xyes"])
])
dnl ==========================================================================
