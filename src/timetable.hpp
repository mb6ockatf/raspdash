/* timetable.hpp -- weekly timetable from ruz.spbstu.ru.

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

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace timetable
{
struct Lesson
{
    std::chrono::minutes start{}, end{};
    std::string subject;
    std::string kind;
    std::vector<std::string> teachers;
    std::string place;
};

struct Day
{
    std::chrono::year_month_day date;
    std::vector<Lesson> lessons;
};

struct Week
{
    std::string group;
    std::chrono::year_month_day first, last;
    bool odd = false;
    std::vector<Day> days;
};

Week parse_week (std::string_view json);

int parse_group_id (std::string_view json, std::string_view name);
}
