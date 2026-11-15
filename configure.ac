AC_PREREQ([2.69])
AC_INIT([nusic], [0.1.0], [nusic@nburch.org], [nusic],
	[https://github.com/noahburchell/nusic])
AC_CONFIG_SRCDIR([src/nusic.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([1.16 foreign subdir-objects no-dist-gzip dist-xz -Wall -Wno-portability])
AM_SILENT_RULES([yes])

AM_MAINTAINER_MODE([disable])

m4_ifndef([PKG_CHECK_MODULES],
	  [m4_fatal([pkg.m4 is missing, install pkgconf])])

: ${CFLAGS=""}
AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
AC_PROG_INSTALL
AC_PROG_MKDIR_P
AC_CANONICAL_HOST
PKG_PROG_PKG_CONFIG([0.29])

AS_CASE([$host_os], [linux*], [], [AC_MSG_ERROR([nusic requires linux, not $host_os])])

AC_CACHE_CHECK([for the C compiler vendor], [nusic_cv_cc_name], [
  AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#ifndef __clang__
#error not clang
#endif
  ]])], [nusic_cv_cc_name=clang], [
    AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#ifndef __GNUC__
#error not gcc
#endif
    ]])], [nusic_cv_cc_name=gcc], [nusic_cv_cc_name=cc])
  ])
])
CC_NAME=$nusic_cv_cc_name
AC_SUBST([CC_NAME])

nusic_std=
NUSIC_TRY_CFLAG([-std=gnu23], [nusic_std])
AS_IF([test -z "$nusic_std"], [NUSIC_TRY_CFLAG([-std=gnu2x], [nusic_std])])
AS_IF([test -z "$nusic_std"],
      [AC_MSG_ERROR([$CC does not support C23, gcc 14+ or clang 18+ is required])])

NUSIC_OPT_CFLAGS=
NUSIC_OPT_LDFLAGS=
NUSIC_CFLAGS="$nusic_std"
NUSIC_CPPFLAGS=
NUSIC_LDFLAGS=

NUSIC_TRY_CFLAGS([
	-Wall
	-Wextra
	-Wpedantic
	-Wno-unused-parameter
	-Wshadow
	-Wundef
	-Wcast-qual
	-Wcast-align
	-Wwrite-strings
	-Wpointer-arith
	-Wmissing-prototypes
	-Wmissing-declarations
	-Wstrict-prototypes
	-Wold-style-definition
	-Wredundant-decls
	-Wnested-externs
	-Wswitch-enum
	-Wformat=2
	-Wvla
	-Wdouble-promotion
	-Wfloat-equal
	-Winit-self
	-Wmissing-include-dirs
	-Wimplicit-fallthrough
	-Wnull-dereference
	-Wdate-time
	-Wmissing-variable-declarations
	-Wstrict-aliasing=3
], [NUSIC_CFLAGS])

AS_IF([test "x$CC_NAME" = xclang], [
  NUSIC_TRY_CFLAGS([
	-Wshadow-all
	-Wassign-enum
	-Wcomma
	-Wconditional-uninitialized
	-Wloop-analysis
	-Wunreachable-code-aggressive
	-Wextra-semi
	-Wover-aligned
	-Wformat-type-confusion
	-Wtautological-constant-in-range-compare
	-Wbitfield-enum-conversion
	-Wcast-function-type-strict
	-Wthread-safety
	-Wheader-guard
	-Wnewline-eof
	-Wstrict-prototypes
  ], [NUSIC_CFLAGS])
], [
  NUSIC_TRY_CFLAGS([
	-Wlogical-op
	-Wduplicated-cond
	-Wduplicated-branches
	-Wjump-misses-init
	-Wtrampolines
	-Walloc-zero
	-Walloca
	-Warray-bounds=2
	-Wshift-overflow=2
	-Wstringop-overflow=4
	-Wstringop-truncation
	-Wformat-overflow=2
	-Wformat-truncation=2
	-Wuse-after-free=3
	-Wflex-array-member-not-at-end
	-Wbidi-chars=any
	-Wdisabled-optimization
	-Wcalloc-transposed-args
	-Wmultistatement-macros
	-Wsuggest-attribute=format
	-Wsuggest-attribute=noreturn
	-Wattribute-alias=2
	-Wtrivial-auto-var-init
  ], [NUSIC_CFLAGS])
])

AC_ARG_ENABLE([werror],
  [AS_HELP_STRING([--enable-werror], [turn warnings into errors])],
  [], [enable_werror=no])
AS_IF([test "x$enable_werror" = xyes], [NUSIC_TRY_CFLAG([-Werror], [NUSIC_CFLAGS])])

AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug], [build unoptimised with ASan and UBSan])],
  [], [enable_debug=no])

AC_ARG_ENABLE([hardened],
  [AS_HELP_STRING([--enable-hardened],
    [_FORTIFY_SOURCE=3, control-flow protection and further hardening])],
  [], [enable_hardened=no])

nusic_have_lto=no
AS_IF([test "x$enable_debug" = xyes], [
  NUSIC_TRY_CFLAGS([-O0 -g3], [NUSIC_OPT_CFLAGS])
  NUSIC_TRY_CFLAGS([-fno-omit-frame-pointer -fno-common], [NUSIC_CFLAGS])
  nusic_san=
  NUSIC_TRY_CFLAGS([
	-fsanitize=address
	-fsanitize=undefined
	-fsanitize-address-use-after-scope
	-fno-sanitize-recover=all
  ], [nusic_san])
  AS_IF([test "x$CC_NAME" = xclang], [
    NUSIC_TRY_CFLAGS([-fsanitize=integer -fsanitize=local-bounds -fno-sanitize=unsigned-integer-overflow],
		     [nusic_san])
  ])
  NUSIC_CFLAGS="$NUSIC_CFLAGS$nusic_san"
  NUSIC_LDFLAGS="$NUSIC_LDFLAGS$nusic_san"
], [
  NUSIC_TRY_CFLAGS([-O2 -pipe -fno-plt], [NUSIC_OPT_CFLAGS])
  NUSIC_TRY_LDFLAG([-Wl,-O2], [NUSIC_OPT_LDFLAGS])
  NUSIC_TRY_LDFLAG([-Wl,--as-needed], [NUSIC_OPT_LDFLAGS])
  NUSIC_TRY_LDFLAG([-Wl,--hash-style=gnu], [NUSIC_OPT_LDFLAGS])
  NUSIC_TRY_LDFLAG_RUN([-Wl,-z,pack-relative-relocs], [NUSIC_OPT_LDFLAGS])

  nusic_lto=
  nusic_lto_CFLAGS=$CFLAGS
  CFLAGS=
  AS_IF([test "x$CC_NAME" = xclang], [
    NUSIC_TRY_LDFLAG([-flto=thin], [nusic_lto])
    AS_IF([test -z "$nusic_lto"], [NUSIC_TRY_LDFLAG([-flto], [nusic_lto])])
  ], [
    NUSIC_TRY_LDFLAG([-flto=auto], [nusic_lto])
    AS_IF([test -z "$nusic_lto"], [NUSIC_TRY_LDFLAG([-flto], [nusic_lto])])
    AS_IF([test -n "$nusic_lto"], [
      NUSIC_TRY_CFLAG([-fno-fat-lto-objects], [nusic_lto])
      NUSIC_TRY_CFLAG([-flto-partition=one], [nusic_lto])
    ])
  ])
  CFLAGS=$nusic_lto_CFLAGS
  AS_IF([test -n "$nusic_lto"], [nusic_have_lto=yes])
  NUSIC_OPT_CFLAGS="$NUSIC_OPT_CFLAGS$nusic_lto"
])

AS_IF([test "x$enable_debug" != xyes], [
  AS_IF([test "x$enable_hardened" = xyes], [nusic_fortify=3], [nusic_fortify=2])
  NUSIC_CPPFLAGS="$NUSIC_CPPFLAGS -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=$nusic_fortify"
])
NUSIC_TRY_CFLAGS([-fstack-protector-strong -fstack-clash-protection -fPIE], [NUSIC_CFLAGS])
AS_CASE([" $LDFLAGS "], [*" -static "*|*" -static-pie "*], [],
	[NUSIC_TRY_LDFLAG([-pie], [NUSIC_LDFLAGS])])
NUSIC_TRY_LDFLAG([-Wl,-z,relro], [NUSIC_LDFLAGS])
NUSIC_TRY_LDFLAG([-Wl,-z,now], [NUSIC_LDFLAGS])
NUSIC_TRY_LDFLAG([-Wl,-z,noexecstack], [NUSIC_LDFLAGS])
AS_IF([test "x$enable_hardened" = xyes], [
  NUSIC_TRY_CFLAGS([
	-fcf-protection=full
	-mbranch-protection=standard
	-ftrivial-auto-var-init=zero
	-fstrict-flex-arrays=3
	-fno-delete-null-pointer-checks
	-fno-strict-overflow
  ], [NUSIC_CFLAGS])
])

# ncurses 6 reports the mouse wheel, ffmpeg 5.1 is the first release with AVChannelLayout
NUSIC_REQUIRE([NCURSES], [ncursesw >= 6.0])
NUSIC_REQUIRE([PIPEWIRE], [libpipewire-0.3])
NUSIC_REQUIRE([FFMPEG], [libavformat >= 59.27 libavcodec >= 59.37 libswresample >= 4.7 libavutil >= 57.28])

nusic_ncurses_version=`$PKG_CONFIG --modversion ncursesw`
nusic_pipewire_version=`$PKG_CONFIG --modversion libpipewire-0.3`
nusic_avformat_version=`$PKG_CONFIG --modversion libavformat`
nusic_avcodec_version=`$PKG_CONFIG --modversion libavcodec`

AC_SUBST([NUSIC_OPT_CFLAGS])
AC_SUBST([NUSIC_OPT_LDFLAGS])
AC_SUBST([NUSIC_CFLAGS])
AC_SUBST([NUSIC_CPPFLAGS])
AC_SUBST([NUSIC_LDFLAGS])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AC_MSG_NOTICE([
  nusic $VERSION

    compiler ........ $CC ($CC_NAME)
    bindir .......... $(eval echo $(eval echo $bindir))

    ncurses ......... $nusic_ncurses_version
    pipewire ........ $nusic_pipewire_version
    ffmpeg .......... libavformat $nusic_avformat_version, libavcodec $nusic_avcodec_version

    lto ............. $nusic_have_lto
    hardened ........ $enable_hardened
    sanitisers ...... $enable_debug

    CFLAGS ..........$NUSIC_OPT_CFLAGS $CFLAGS$NUSIC_CPPFLAGS$NUSIC_CFLAGS
    LDFLAGS .........$NUSIC_OPT_LDFLAGS $LDFLAGS$NUSIC_LDFLAGS
])
