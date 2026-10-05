# configure.ac -- autoconf input for raspdash.
#
# Copyright (C) 2026 mb6ockatf
#
# This file is part of raspdash.
#
# raspdash is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.

AC_PREREQ([2.69])
AC_INIT([raspdash], [0.1.0], [mdddmmmm@outlook.com], [raspdash],
        [https://github.com/mb6ockatf/raspdash])
AC_CONFIG_SRCDIR([src/main.cpp])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([gnits subdir-objects -Wall -Werror])
AM_SILENT_RULES([yes])

AC_PROG_CXX
AM_PROG_AR
AC_PROG_RANLIB
AC_LANG([C++])

# C++23 without GNU extensions.  autoconf-archive's AX_CXX_COMPILE_STDCXX
# on older distributions stops at C++17, so check by hand.
AC_CACHE_CHECK([for $CXX option to enable C++23], [rd_cv_cxx23], [
  rd_cv_cxx23=no
  rd_save_CXX=$CXX
  for rd_flag in '' -std=c++23 -std=c++2b; do
    CXX="$rd_save_CXX $rd_flag"
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#if __cplusplus < 202302L
#error not C++23
#endif
#include <expected>
#include <print>
#include <ranges>
#include <vector>
]], [[std::expected<int, int> e = 1;
std::println ("{}", *e);
auto v = std::views::iota (0, 3) | std::ranges::to<std::vector<int>> ();
return v.size () == 3 ? 0 : 1;]])],
      [rd_cv_cxx23=${rd_flag:-none needed}; break])
  done
  CXX=$rd_save_CXX])
AS_CASE([$rd_cv_cxx23],
  [no], [AC_MSG_ERROR([a C++23 compiler with <print> and std::ranges::to is required (GCC 14 or later)])],
  ['none needed'], [],
  [CXX="$CXX $rd_cv_cxx23"])

AX_PTHREAD([], [AC_MSG_ERROR([pthreads are required])])

PKG_PROG_PKG_CONFIG

# Static libraries (such as the private build in /opt/raspdash-deps) need
# their own dependencies on the link line, which only 'pkg-config --static'
# lists: static libcurl needs -lssl -lcrypto -lz.
AC_ARG_ENABLE([static-deps],
  [AS_HELP_STRING([--enable-static-deps],
    [link ncursesw, libcurl and libcrypto statically, using pkg-config --static])],
  [], [enable_static_deps=no])
AS_IF([test "x$enable_static_deps" = xyes],
  [PKG_CONFIG="$PKG_CONFIG --static"])

PKG_CHECK_MODULES([NCURSES], [ncursesw])
PKG_CHECK_MODULES([CURL], [libcurl])
PKG_CHECK_MODULES([CRYPTO], [libcrypto])

# Prefer a pkg-config file (a private nlohmann/json build ships one); fall
# back to the system header.
PKG_CHECK_MODULES([JSON], [nlohmann_json >= 3.11], [],
  [AC_CHECK_HEADER([nlohmann/json.hpp], [],
    [AC_MSG_ERROR([nlohmann/json.hpp not found (install nlohmann-json3-dev)])])])

# A GCC newer than the system's needs its libstdc++ linked in statically,
# or the binary will not find the newer library at run time.
AC_ARG_ENABLE([static-libstdcxx],
  [AS_HELP_STRING([--enable-static-libstdcxx],
    [link libstdc++ and libgcc statically (for a compiler newer than the system)])],
  [], [enable_static_libstdcxx=no])
# libatomic belongs to the compiler too: static OpenSSL asks for -latomic,
# so take the compiler's libatomic.a rather than its shared library.
AS_IF([test "x$enable_static_libstdcxx" = xyes],
  [AC_SUBST([STATIC_CXX_LDFLAGS], ["-static-libstdc++ -static-libgcc"])
   NCURSES_LIBS=`echo "$NCURSES_LIBS" | sed 's/-latomic\>/-l:libatomic.a/g'`
   CURL_LIBS=`echo "$CURL_LIBS" | sed 's/-latomic\>/-l:libatomic.a/g'`
   CRYPTO_LIBS=`echo "$CRYPTO_LIBS" | sed 's/-latomic\>/-l:libatomic.a/g'`])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
