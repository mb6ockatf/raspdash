# Makefile.am -- automake input for raspdash.
#
# Copyright (C) 2026 mb6ockatf
#
# This file is part of raspdash.
#
# raspdash is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.

ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = \
	-I$(top_srcdir)/src -I$(top_srcdir)/third_party \
	-DSYSCONFDIR='"$(sysconfdir)"' \
	-DPKGDATADIR='"$(pkgdatadir)"' \
	-DTOML_HEADER_ONLY=0
AM_CXXFLAGS = -Wall -Wextra \
	$(PTHREAD_CFLAGS) $(NCURSES_CFLAGS) $(CURL_CFLAGS) $(CRYPTO_CFLAGS) \
	$(JSON_CFLAGS)
AM_LDFLAGS = $(STATIC_CXX_LDFLAGS)
LDADD = libraspdash.a \
	$(NCURSES_LIBS) $(CURL_LIBS) $(CRYPTO_LIBS) $(PTHREAD_LIBS)

# Everything except the two main()s, so tests can link against it.
noinst_LIBRARIES = libraspdash.a
libraspdash_a_SOURCES = \
	src/app.cpp src/app.hpp \
	src/config.cpp src/config.hpp \
	src/http.cpp src/http.hpp \
	src/modules.hpp \
	src/screen.hpp \
	src/source.cpp src/source.hpp \
	src/tui.cpp src/tui.hpp \
	src/util.cpp src/util.hpp \
	src/machines.cpp src/machines.hpp \
	src/tasks.cpp src/tasks.hpp \
	src/timetable.cpp src/timetable.hpp \
	src/weather.cpp src/weather.hpp \
	src/markets.cpp src/markets.hpp

bin_PROGRAMS = src/raspdash src/raspdash-auth
src_raspdash_SOURCES = src/main.cpp
src_raspdash_auth_SOURCES = src/auth_main.cpp

dist_pkgdata_DATA = agent/raspdash-stats.sh config.example.toml

info_TEXINFOS = doc/raspdash.texi
doc_raspdash_TEXINFOS = \
	doc/machines.texi doc/tasks.texi doc/timetable.texi \
	doc/weather.texi doc/markets.texi

check_PROGRAMS = \
	tests/test_util \
	tests/test_timetable \
	tests/test_machines \
	tests/test_tasks \
	tests/test_weather \
	tests/test_markets
TESTS = $(check_PROGRAMS)
AM_TESTS_ENVIRONMENT = srcdir='$(srcdir)'; export srcdir;
tests_test_util_SOURCES = tests/test_util.cpp tests/check.hpp
tests_test_timetable_SOURCES = tests/test_timetable.cpp tests/check.hpp
tests_test_machines_SOURCES = tests/test_machines.cpp tests/check.hpp
tests_test_tasks_SOURCES = tests/test_tasks.cpp tests/check.hpp
tests_test_weather_SOURCES = tests/test_weather.cpp tests/check.hpp
tests_test_markets_SOURCES = tests/test_markets.cpp tests/check.hpp

EXTRA_DIST = \
	bootstrap \
	hooks/pre-commit \
	deploy \
	secrets/README.md \
	tests/data \
	third_party/toml.hpp third_party/toml++-LICENSE
