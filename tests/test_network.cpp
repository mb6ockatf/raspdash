/* test_network.cpp -- tests for the Network screen.

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

#include "check.hpp"
#include "network.hpp"
#include "util.hpp"

int
main ()
{
    return check::run ([]
    {
        CHECK (network::parse_address ("192.168.1.96") == 0xC0A80160u);
        CHECK (network::format_address (0xC0A80101u) == "192.168.1.1");

        auto targets = network::subnet_targets (0xC0A80160u, 24);
        CHECK (targets.size () == 254);
        CHECK (targets.front () == 0xC0A80101u && targets.back () == 0xC0A801FEu);
        CHECK (network::subnet_targets (0x0A000005u, 8).size () == 254);
        CHECK (network::subnet_targets (0xC0A80160u, 23).size () == 510);

        auto neighbours = network::parse_arp (
            "IP address       HW type     Flags       HW address            Mask     Device\n"
            "192.168.1.1      0x1         0x2         50:ff:20:aa:bb:cc     *        wlan0\n"
            "192.168.1.44     0x1         0x0         00:00:00:00:00:00     *        wlan0\n"
            "192.168.1.90     0x1         0x2         a4:c3:f0:11:22:33     *        wlan0\n");
        CHECK (neighbours.size () == 2);
        CHECK (neighbours.at (0xC0A80101u) == "50:ff:20:aa:bb:cc");

        CHECK (network::ssh_detail ("SSH-2.0-OpenSSH_10.0p2 Debian-7\r\n")
               == "OpenSSH_10.0p2 Debian-7");
        CHECK (network::http_detail ("HTTP/1.1 200 OK\r\nServer: nginx/1.26\r\n\r\n")
               == "nginx/1.26");
        CHECK (network::http_detail ("HTTP/1.0 404 Not Found\r\n\r\n") == "404");

        CHECK (network::services_text ({{22, "ssh", "OpenSSH_9.2p1"}, {80, "http", ""}})
        == "ssh:22 (OpenSSH_9.2p1) · http:80");

        network::Scan scan;
        scan.interface = "wlan0";
        scan.subnet = "192.168.1.0/24";
        scan.started = util::TimePoint{std::chrono::seconds{1791154000}};
        scan.took = std::chrono::milliseconds{9800};
        network::Host host;
        host.address = "192.168.1.90";
        host.address_bits = 0xC0A8015Au;
        host.name = "laptop";
        host.is_self = true;
        host.services.push_back ({22, "ssh", "OpenSSH"});
        scan.hosts.push_back (host);
        network::Scan back = network::scan_from_json (network::scan_to_json (scan));
        CHECK (back.subnet == scan.subnet && back.started == scan.started);
        CHECK (back.took == scan.took && back.hosts.size () == 1);
        CHECK (back.hosts[0].address_bits == 0xC0A8015Au && back.hosts[0].is_self);
        CHECK (back.hosts[0].services.at (0).detail == "OpenSSH");
    });
}
