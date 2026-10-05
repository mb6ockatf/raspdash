/* util.cpp -- small helpers shared by every module.

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

#include "util.hpp"

#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace util
{
const std::chrono::time_zone *
zone (std::string_view name)
{
    if (name.empty ())
        return std::chrono::current_zone ();
    return std::chrono::locate_zone (name);
}

std::chrono::local_seconds
to_local (TimePoint tp, const std::chrono::time_zone *z)
{
    return z->to_local (std::chrono::floor<std::chrono::seconds> (tp));
}

namespace
{

bool
digits (std::string_view s, std::size_t &pos, std::size_t n, int &out)
{
    if (pos + n > s.size ())
        return false;
    auto r = std::from_chars (s.data () + pos, s.data () + pos + n, out);
    if (r.ec != std::errc () || r.ptr != s.data () + pos + n)
        return false;
    pos += n;
    return true;
}

bool
expect (std::string_view s, std::size_t &pos, char c)
{
    if (pos >= s.size () || s[pos] != c)
        return false;
    ++pos;
    return true;
}
}

std::optional<TimePoint>
parse_iso8601 (std::string_view s)
{
    using namespace std::chrono;
    std::size_t p = 0;
    int Y, M, D, h = 0, m = 0, sec = 0;
    if (!digits (s, p, 4, Y) || !expect (s, p, '-')
            || !digits (s, p, 2, M) || !expect (s, p, '-')
            || !digits (s, p, 2, D))
        return std::nullopt;
    if (p < s.size () && (s[p] == 'T' || s[p] == ' '))
        {
            ++p;
            if (!digits (s, p, 2, h) || !expect (s, p, ':')
                    || !digits (s, p, 2, m))
                return std::nullopt;
            if (p < s.size () && s[p] == ':' && !(++p, digits (s, p, 2, sec)))
                return std::nullopt;

            if (p < s.size () && s[p] == '.')
                while (++p < s.size () && s[p] >= '0' && s[p] <= '9')
                    ;
        }
    seconds offset{0};
    if (p < s.size ())
        {
            char c = s[p];
            if (c == 'Z')
                ++p;
            else if (c == '+' || c == '-')
                {
                    ++p;
                    int oh, om = 0;
                    if (!digits (s, p, 2, oh))
                        return std::nullopt;
                    if (p < s.size () && s[p] == ':')
                        ++p;
                    if (p < s.size () && !digits (s, p, 2, om))
                        return std::nullopt;
                    offset = hours{oh} + minutes{om};
                    if (c == '-')
                        offset = -offset;
                }
            if (p != s.size ())
                return std::nullopt;
        }
    year_month_day ymd{year{Y}, month{static_cast<unsigned> (M)},
                       day{static_cast<unsigned> (D)}};
    if (!ymd.ok () || h > 23 || m > 59 || sec > 60)
        return std::nullopt;
    return sys_days{ymd} + hours{h} + minutes{m} + seconds{sec} - offset;
}

std::string
format_age (TimePoint then, TimePoint now)
{
    auto s = std::chrono::duration_cast<std::chrono::seconds> (now - then)
             .count ();
    if (s < 0)
        s = 0;
    if (s < 60)
        return std::format ("{}s", s);
    if (s < 3600)
        return std::format ("{}m", s / 60);
    if (s < 86400)
        return std::format ("{}h", s / 3600);
    return std::format ("{}d", s / 86400);
}

std::string
human_bytes (std::uint64_t bytes)
{
    static constexpr char units[] = "BKMGTP";
    double v = static_cast<double> (bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5)
        {
            v /= 1024.0;
            ++u;
        }
    if (u == 0)
        return std::format ("{}B", bytes);
    return v < 10 ? std::format ("{:.1f}{}", v, units[u])
           : std::format ("{:.0f}{}", v, units[u]);
}

std::string_view
trim (std::string_view s)
{
    constexpr std::string_view ws = " \t\r\n";
    auto b = s.find_first_not_of (ws);
    if (b == std::string_view::npos)
        return {};
    auto e = s.find_last_not_of (ws);
    return s.substr (b, e - b + 1);
}

std::vector<std::string_view>
split (std::string_view s, char sep)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;)
        {
            auto pos = s.find (sep, start);
            out.push_back (s.substr (start, pos - start));
            if (pos == std::string_view::npos)
                break;
            start = pos + 1;
        }
    return out;
}

std::vector<Interface>
ipv4_interfaces ()
{
    std::vector<Interface> found;
    ifaddrs *list = nullptr;
    if (getifaddrs (&list) != 0)
        return found;
    for (ifaddrs *entry = list; entry; entry = entry->ifa_next)
        {
            if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET
                    || !(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK))
                continue;
            const auto *address = reinterpret_cast<const sockaddr_in *> (entry->ifa_addr);
            Interface interface;
            interface.name = entry->ifa_name;
            interface.address_bits = ntohl (address->sin_addr.s_addr);
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop (AF_INET, &address->sin_addr, text, sizeof text);
            interface.address = text;
            if (entry->ifa_netmask)
                {
                    std::uint32_t mask = ntohl (reinterpret_cast<const sockaddr_in *>
                                                (entry->ifa_netmask)->sin_addr.s_addr);
                    while (mask & 0x80000000u)
                        {
                            ++interface.prefix_length;
                            mask <<= 1;
                        }
                }
            found.push_back (std::move (interface));
        }
    freeifaddrs (list);
    return found;
}

std::string
default_route_interface (std::string_view proc_net_route)
{
    for (std::string_view line : split (proc_net_route, '\n'))
        {
            std::vector<std::string_view> fields;
            for (std::string_view field : split (line, '\t'))
                if (!trim (field).empty ())
                    fields.push_back (trim (field));
            if (fields.size () > 2 && fields[1] == "00000000" && fields[0] != "Iface")
                return std::string (fields[0]);
        }
    return {};
}

std::optional<Interface>
primary_ipv4 ()
{
    std::vector<Interface> interfaces = ipv4_interfaces ();
    if (interfaces.empty ())
        return std::nullopt;
    std::ifstream route ("/proc/net/route");
    std::string table ((std::istreambuf_iterator<char> (route)),
                       std::istreambuf_iterator<char> ());
    std::string preferred = default_route_interface (table);
    for (const Interface &interface : interfaces)
        if (interface.name == preferred)
            return interface;
    return interfaces.front ();
}
}
