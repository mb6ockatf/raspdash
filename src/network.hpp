/* network.hpp -- the Network screen: hosts and services on the LAN.

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
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace network
{
struct Port
{
    int number = 0;
    std::string name;
};

struct Service
{
    int port = 0;
    std::string name;
    std::string detail;
};

struct Host
{
    std::uint32_t address_bits = 0;
    std::string address;
    std::string name;
    std::string hostname;
    std::string mac;
    bool is_self = false;
    std::vector<Service> services;
};

struct Scan
{
    std::string interface;
    std::string subnet;
    util::TimePoint started{};
    std::chrono::milliseconds took{};
    std::vector<Host> hosts;
};

std::vector<Port> default_ports ();

std::string format_address (std::uint32_t address_bits);

std::uint32_t parse_address (std::string_view text);

std::vector<std::uint32_t> subnet_targets (std::uint32_t address_bits,
        int prefix_length,
        int smallest_prefix = 22);

std::map<std::uint32_t, std::string> parse_arp (std::string_view proc_net_arp);

std::string ssh_detail (std::string_view banner);

std::string http_detail (std::string_view response);

std::string services_text (const std::vector<Service> &services);

std::string scan_to_json (const Scan &scan);

Scan scan_from_json (std::string_view text);
}
