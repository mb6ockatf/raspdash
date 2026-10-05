/* util.hpp -- small helpers shared by every module.

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
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace util
{
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

const std::chrono::time_zone *zone (std::string_view name = {});

std::chrono::local_seconds to_local (TimePoint tp,
                                     const std::chrono::time_zone *z);

std::optional<TimePoint> parse_iso8601 (std::string_view s);

std::string format_age (TimePoint then, TimePoint now = Clock::now ());

std::string human_bytes (std::uint64_t bytes);

std::string_view trim (std::string_view s);
std::vector<std::string_view> split (std::string_view s, char sep);
}
