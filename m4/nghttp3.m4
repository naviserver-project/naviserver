dnl ==========================================================================
dnl AX_CHECK_NGHTTP3 -- discover nghttp3 flags (pkg-config optional)
dnl
dnl --with-nghttp3 values:
dnl   yes (default)   -> try pkg-config if available; else fall back
dnl   no              -> skip detection
dnl   PREFIX          -> use -I PREFIX/include, -L PREFIX/lib (and fallback probe)
dnl   pkgconfig       -> REQUIRE pkg-config; fail if macros/binary missing
dnl ==========================================================================

AC_DEFUN([AX_CHECK_NGHTTP3], [
  AC_ARG_WITH([nghttp3],
    [AS_HELP_STRING([--with-nghttp3@<:@=PREFIX|pkgconfig|no@:>@],
                    [nghttp3 prefix; "pkgconfig" to require pkg-config])],
    [with_nghttp3="$withval"], [with_nghttp3="yes"])

  have_nghttp3="no"
  ax_nghttp3_strict="no"
  AS_CASE([$with_nghttp3],
    [pkgconfig|pkgconf|required], [ax_nghttp3_strict="yes"],
    [yes|no|*], [:])

  dnl Respect explicit env overrides first
  if test "x$NGHTTP3_CFLAGS" != x -o "x$NGHTTP3_LIBS" != x ; then
    have_nghttp3="yes"
  fi

  dnl If a PREFIX was given, prime hints (work with/without pkg-config)
  if test "x$with_nghttp3" != "xyes" -a "x$with_nghttp3" != "xno" \
       -a "x$with_nghttp3" != "xpkgconfig" -a "x$with_nghttp3" != "xpkgconf" \
       -a "x$with_nghttp3" != "xrequired" ; then
    test "x$NGHTTP3_CFLAGS" = x && NGHTTP3_CFLAGS="-I$with_nghttp3/include"
    test "x$NGHTTP3_LIBS"   = x && NGHTTP3_LIBS="-L$with_nghttp3/lib -lnghttp3"
  fi

  dnl --- Try pkg-config if requested/allowed and macros are available ------
  if test "x$have_nghttp3" = "xno" -a "x$with_nghttp3" != "xno"; then
    m4_ifdef([PKG_CHECK_MODULES], [
      PKG_PROG_PKG_CONFIG
      if test "x$PKG_CONFIG" != "x"; then
        dnl Distros vary: .pc can be libnghttp3 or nghttp3
        PKG_CHECK_MODULES([NGHTTP3], [libnghttp3], [have_nghttp3="yes"], [
          PKG_CHECK_MODULES([NGHTTP3], [nghttp3], [have_nghttp3="yes"], [:])
        ])
      elif test "x$ax_nghttp3_strict" = "xyes"; then
        AC_MSG_ERROR([pkg-config binary not found but --with-nghttp3=pkgconfig was requested])
      fi
    ], [
      dnl pkg.m4 not loaded; only fail if strictly required
      if test "x$ax_nghttp3_strict" = "xyes"; then
        AC_MSG_ERROR([pkg-config m4 macros (pkg.m4) not available but --with-nghttp3=pkgconfig was requested.
Install pkgconfig/pkgconf and ensure pkg.m4 is in aclocal's path, or use --with-nghttp3=PREFIX])
      else
        AC_MSG_NOTICE([pkg-config macros not available; skipping pkg-config detection for nghttp3])
      fi
    ])
  fi

  dnl --- Fallback: header + symbol probe (honors any *_CFLAGS/*_LIBS set) ---
  if test "x$have_nghttp3" = "xno" -a "x$with_nghttp3" != "xno"; then
    save_CFLAGS="$CFLAGS"; save_LIBS="$LIBS"
    CFLAGS="$CFLAGS $NGHTTP3_CFLAGS"
    LIBS="$LIBS $NGHTTP3_LIBS"

    AC_CHECK_HEADERS([nghttp3/nghttp3.h], [
      AC_CHECK_LIB([nghttp3], [nghttp3_conn_client_new], [
        have_nghttp3="yes"
        test "x$NGHTTP3_LIBS" = "x" && NGHTTP3_LIBS="-lnghttp3"
      ])
    ])

    CFLAGS="$save_CFLAGS"; LIBS="$save_LIBS"
  fi

  AS_IF([test "x$have_nghttp3" = "xyes"], [
    AC_DEFINE([HAVE_NGHTTP3], [1], [Define if nghttp3 is available])
  ])

  AC_SUBST([NGHTTP3_CFLAGS])
  AC_SUBST([NGHTTP3_LIBS])
  AM_CONDITIONAL([HAVE_NGHTTP3], [test "x$have_nghttp3" = "xyes"])

  dnl Summary
  if test "x$have_nghttp3" = "xyes"; then
    AC_MSG_RESULT([nghttp3: yes])
    AC_MSG_NOTICE([NGHTTP3_CFLAGS: $NGHTTP3_CFLAGS])
    AC_MSG_NOTICE([NGHTTP3_LIBS:   $NGHTTP3_LIBS])
  else
    AC_MSG_RESULT([nghttp3: no])
  fi
])
