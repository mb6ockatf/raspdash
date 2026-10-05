/* config.hpp -- the TOML configuration file.

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

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <toml.hpp>

class Config
{
public:

    static Config load (const std::filesystem::path &path);

    static std::optional<std::filesystem::path>
    find (std::string_view cli_path = {});

    const toml::table &root () const
    {
        return root_;
    }

    const toml::table &section (std::string_view name) const;

    std::filesystem::path resolve (std::string_view p) const;

    const std::filesystem::path &path () const
    {
        return path_;
    }

    std::string user_agent () const;

private:
    toml::table root_;
    std::filesystem::path path_;
};
