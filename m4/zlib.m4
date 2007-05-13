#------------------------------------------------------------------------
# AC_CHECK_ZLIB --
#
#       Set the zlib compile flags, possibly using a special directory.
#
# Arguments:
#       none
#
# Results:
#
#       Adds the following arguments to configure:
#               --with-zlib=[dir]
#
#       Defines the following vars:
#               ZLIB_INCLUDES   Full path to the directory containing
#                               the zlib.h file if a zlib directory
#                               was specified.
#               ZLIB_LIBS       Linker line for libz.
#------------------------------------------------------------------------

AC_DEFUN([AX_CHECK_ZLIB], [
AC_MSG_CHECKING([for zlib compression library])
AC_ARG_WITH([zlib],
  AS_HELP_STRING(--with-zlib=DIR,Build and link with Zlib),
  [
    ac_zlib=$withval
    if test "${ac_zlib}" != "no" ; then
      ac_zlib=yes
      if test -d "$withval" ; then
        ZLIB_INCLUDES="-I$withval/include"
        ZLIB_LIBS="-L$withval/lib -lz"
      fi
    fi
  ],
  [
    ac_zlib="yes"
    ZLIB_INCLUDES=""
    ZLIB_LIBS="-lz"
  ])
AC_MSG_RESULT($ac_zlib)

if test "${ac_zlib}" = "yes" ; then
  save_CPPFLAGS="$CPPFLAGS"
  save_LIBS="$LIBS"
  CPPFLAGS="$ZLIB_INCLUDES $CPPFLAGS"
  LIBS="$LIBS $ZLIB_LIBS"

  AC_CHECK_HEADERS(zlib.h)
  AC_CHECK_LIB(z, compress2)

  if test "${ac_cv_header_zlib_h}" != "yes" -o "${ac_cv_lib_z_compress2}" != "yes" ; then
    AC_MSG_ERROR([Zlib compression support requested but not available])
  fi

  CPPFLAGS="$save_CPPFLAGS"
  LIBS="$save_LIBS"
fi

AC_SUBST([ZLIB_INCLUDES])
AC_SUBST([ZLIB_LIBS])

])
