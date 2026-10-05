/* check.hpp -- a tiny test harness for 'make check'.

   Copyright (C) 2026 mb6ockatf

   This file is part of raspdash.

   raspdash is free software: you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation, either version 3 of the License, or (at your
   option) any later version.

   raspdash is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with raspdash.  If not, see <https://www.gnu.org/licenses/>.  */

#pragma once

#include <cstdlib>
#include <exception>
#include <fstream>
#include <print>
#include <source_location>
#include <sstream>
#include <string>

namespace check
{
inline int failures = 0;

inline void
expect (bool ok, const char *what,
        std::source_location loc = std::source_location::current ())
{
    if (!ok)
        {
            std::println (stderr, "{}:{}: check failed: {}", loc.file_name (),
                          loc.line (), what);
            ++failures;
        }
}

inline std::string
data (const std::string &name)
{
    const char *srcdir = std::getenv ("srcdir");
    std::string path = std::string (srcdir ? srcdir : ".") + "/tests/data/"
                       + name;
    std::ifstream in (path, std::ios::binary);
    if (!in)
        {
            std::println (stderr, "cannot open {}", path);
            std::exit (99);
        }
    std::ostringstream ss;
    ss << in.rdbuf ();
    return ss.str ();
}

template <typename F>
int
run (F body)
{
    try
        {
            body ();
        }
    catch (const std::exception &e)
        {
            std::println (stderr, "unexpected exception: {}", e.what ());
            return 99;
        }
    return failures ? 1 : 0;
}
}

#define CHECK(cond) ::check::expect ((cond), #cond)
