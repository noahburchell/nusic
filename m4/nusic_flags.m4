AC_DEFUN([NUSIC_TRY_CFLAG], [
  AS_VAR_PUSHDEF([nusic_cv], [nusic_cv_cflag_$1])
  AC_CACHE_CHECK([whether $CC accepts $1], [nusic_cv], [
    nusic_save_CFLAGS=$CFLAGS
    CFLAGS="-Werror $1"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
      [AS_VAR_SET([nusic_cv], [yes])], [AS_VAR_SET([nusic_cv], [no])])
    CFLAGS=$nusic_save_CFLAGS
  ])
  AS_VAR_IF([nusic_cv], [yes], [$2="${$2} $1"])
  AS_VAR_POPDEF([nusic_cv])
])

AC_DEFUN([NUSIC_TRY_CFLAGS], [
  m4_foreach_w([nusic_flag], [$1], [NUSIC_TRY_CFLAG(nusic_flag, [$2])])
])

AC_DEFUN([NUSIC_TRY_LDFLAG], [
  AS_VAR_PUSHDEF([nusic_cv], [nusic_cv_ldflag_$1])
  AC_CACHE_CHECK([whether the linker accepts $1], [nusic_cv], [
    nusic_save_LDFLAGS=$LDFLAGS
    LDFLAGS="$LDFLAGS $1"
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
      [AS_VAR_SET([nusic_cv], [yes])], [AS_VAR_SET([nusic_cv], [no])])
    LDFLAGS=$nusic_save_LDFLAGS
  ])
  AS_VAR_IF([nusic_cv], [yes], [$2="${$2} $1"])
  AS_VAR_POPDEF([nusic_cv])
])

AC_DEFUN([NUSIC_TRY_LDFLAG_RUN], [
  AS_VAR_PUSHDEF([nusic_cv], [nusic_cv_ldrun_$1])
  AC_CACHE_CHECK([whether $1 produces a program that runs], [nusic_cv], [
    nusic_save_LDFLAGS=$LDFLAGS
    LDFLAGS="$LDFLAGS $1"
    AC_RUN_IFELSE([AC_LANG_PROGRAM([[]], [[return 0;]])],
      [AS_VAR_SET([nusic_cv], [yes])],
      [AS_VAR_SET([nusic_cv], [no])],
      [AS_VAR_SET([nusic_cv], [no])])
    LDFLAGS=$nusic_save_LDFLAGS
  ])
  AS_VAR_IF([nusic_cv], [yes], [$2="${$2} $1"])
  AS_VAR_POPDEF([nusic_cv])
])

# NUSIC_REQUIRE(VARIABLE-PREFIX, MODULES)
# the headers of a dependency are system headers, the warning set does not apply to them
AC_DEFUN([NUSIC_REQUIRE], [
  PKG_CHECK_MODULES([$1], [$2])
  $1[]_CFLAGS=`AS_ECHO(["$$1[]_CFLAGS"]) | sed -e 's/^-I/-isystem /' -e 's/ -I/ -isystem /g'`
])
