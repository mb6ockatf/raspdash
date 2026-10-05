/* modules.hpp -- the screens raspdash knows how to build.

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

#include <string_view>

class App;

using InstallFn = void (*) (App &app);

struct Module
{
    std::string_view name;
    InstallFn install;
};

void install_machines (App &app);
void install_tasks (App &app);
void install_timetable (App &app);
void install_weather (App &app);
void install_markets (App &app);
void install_network (App &app);
void install_uptime (App &app);

inline constexpr Module modules[] =
{
    {"machines", install_machines},
    {"uptime", install_uptime},
    {"tasks", install_tasks},
    {"timetable", install_timetable},
    {"weather", install_weather},
    {"markets", install_markets},
    {"network", install_network},
};
