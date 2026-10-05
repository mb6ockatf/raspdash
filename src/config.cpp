/* config.cpp -- the TOML configuration file.

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

#include <config.h>

#define TOML_IMPLEMENTATION
#include "config.hpp"

#include <cstdlib>
#include <format>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

Config
Config::load (const fs::path &path)
{
    Config c;

    std::error_code ec;
    c.path_ = fs::canonical (path, ec);
    if (ec)
        c.path_ = fs::absolute (path);
    try
        {
            c.root_ = toml::parse_file (c.path_.string ());
        }
    catch (const toml::parse_error &e)
        {
            std::ostringstream where;
            where << e.source ().begin;
            throw std::runtime_error (std::format ("{}:{}: {}", c.path_.string (),
                                                   where.str (),
                                                   e.description ()));
        }
    return c;
}

std::optional<fs::path>
Config::find (std::string_view cli_path)
{
    if (!cli_path.empty ())
        return fs::path (cli_path);
    if (const char *env = std::getenv ("RASPDASH_CONFIG"); env && *env)
        return fs::path (env);

    std::vector<fs::path> candidates{"config.toml"};
    if (const char *xdg = std::getenv ("XDG_CONFIG_HOME"); xdg && *xdg)
        candidates.push_back (fs::path (xdg) / "raspdash" / "config.toml");
    else if (const char *home = std::getenv ("HOME"); home && *home)
        candidates.push_back (fs::path (home) / ".config" / "raspdash"
                              / "config.toml");
    candidates.push_back (fs::path (SYSCONFDIR) / "raspdash" / "config.toml");

    for (const auto &p : candidates)
        {
            std::error_code ec;
            if (fs::is_regular_file (p, ec))
                return p;
        }
    return std::nullopt;
}

const toml::table &
Config::section (std::string_view name) const
{
    static const toml::table empty;
    if (const toml::table *t = root_[name].as_table ())
        return *t;
    return empty;
}

fs::path
Config::resolve (std::string_view p) const
{
    std::string s (p);
    if (s.starts_with ("~/"))
        if (const char *home = std::getenv ("HOME"))
            s = std::string (home) + s.substr (1);
    fs::path out (s);
    if (out.is_relative ())
        out = path_.parent_path () / out;
    return out.lexically_normal ();
}

std::string
Config::user_agent () const
{
    return section ("app")["user_agent"].value_or<std::string> (
               PACKAGE_NAME "/" PACKAGE_VERSION
               " (+https://github.com/mb6ockatf/raspdash)");
}
