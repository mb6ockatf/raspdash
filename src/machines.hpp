/* machines.hpp -- load, memory and temperatures of machines on the LAN.

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
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util.hpp"

class App;
class Source;

namespace machines
{
enum class Kind { local, ssh, keenetic };

struct Temp
{
    std::string chip;
    std::string label;
    double celsius = 0;
};

struct Stats
{
    std::string hostname, os, kernel;
    std::optional<double> uptime_s;
    int ncpu = 0;
    std::optional<double> load1, load5, load15;
    std::optional<double> cpu_pct;
    std::optional<std::uint64_t> mem_total, mem_avail;
    std::optional<std::uint64_t> swap_total, swap_used;
    std::optional<std::uint64_t> disk_total, disk_used;
    std::vector<Temp> temps;
    std::optional<int> battery_pct;
    std::string battery_status;
};

Stats parse_stats (std::string_view text);

Stats parse_keenetic (std::string_view system_json,
                      std::string_view version_json = {});

std::string keenetic_password (std::string_view login, std::string_view realm,
                               std::string_view password,
                               std::string_view challenge);

std::string md5_hex (std::string_view data);
std::string sha256_hex (std::string_view data);

struct CommandResult
{
    int status = -1;
    bool timed_out = false;
    std::string out, err;
};

struct Reading
{
    std::shared_ptr<const Stats> stats;
    util::TimePoint taken{};
    std::chrono::steady_clock::time_point taken_steady{};
};

struct Host
{
    std::string name;
    Kind kind = Kind::ssh;
    std::string address;
    Source *source = nullptr;
    std::function<Reading ()> read;
};

const std::vector<Host> &hosts (App &app);

std::string_view kind_name (Kind kind);

CommandResult run_command (const std::vector<std::string> &argv,
                           std::string_view input,
                           std::chrono::milliseconds timeout);
}
