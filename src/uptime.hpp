/* uptime.hpp -- boot history of the machines and power-cut detection.

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
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace uptime
{
struct Observation
{
    util::TimePoint taken{};
    std::chrono::steady_clock::time_point taken_steady{};
    double uptime_s = 0;
};

struct HostHistory
{
    std::string kind;
    bool battery = false;
    std::vector<util::TimePoint> boots;
    std::optional<Observation> last;
};

using History = std::map<std::string, HostHistory, std::less<>>;

struct Settings
{
    std::size_t keep = 50;
    std::chrono::seconds same_boot{180};
    std::chrono::seconds window{600};
};

bool observe (HostHistory &host, const Observation &observation,
              const Settings &settings = {});

struct Boot
{
    std::string host;
    util::TimePoint at{};
    bool router = false;
    bool battery = false;
};

struct PowerEvent
{
    util::TimePoint at{};
    std::vector<Boot> boots;
    bool router = false;
};

std::vector<Boot> all_boots (const History &history);

std::vector<PowerEvent> power_events (const History &history,
                                      const Settings &settings = {});

std::string event_hosts (const PowerEvent &event);

enum class Cell { unknown, up, down, boot, cut };

struct Lane
{
    std::vector<util::TimePoint> boots;
    std::vector<util::TimePoint> cuts;
    util::TimePoint last_seen{};
    bool up_now = false;
};

std::vector<Cell> timeline (const Lane &lane, util::TimePoint from,
                            util::TimePoint to, int columns);

std::string format_duration (std::chrono::seconds duration);

std::string format_when (util::TimePoint when,
                         const std::chrono::time_zone *zone);

std::string history_to_json (const History &history);

History history_from_json (std::string_view text);
}
