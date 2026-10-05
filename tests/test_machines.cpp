/* test_machines.cpp -- stats parsing, Keenetic login hash, subprocesses.

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

#include <csignal>

#include "check.hpp"
#include "machines.hpp"

using namespace std::chrono;

int
main ()
{
    std::signal (SIGPIPE, SIG_IGN);
    return check::run ([]
    {
        using namespace machines;

        Stats l = parse_stats (check::data ("machines-linux-laptop.txt"));
        CHECK (l.hostname == "laptop");
        CHECK (l.os == "Debian GNU/Linux 13 (trixie)");
        CHECK (l.ncpu == 8);
        CHECK (l.load1 && *l.load1 > 5.5 && *l.load1 < 5.6);
        CHECK (l.cpu_pct && *l.cpu_pct > 99);
        CHECK (l.mem_total == 16000000000ULL);
        CHECK (l.mem_avail == 9000000000ULL);
        CHECK (l.disk_total && l.disk_used && *l.disk_used < *l.disk_total);
        CHECK (l.battery_pct == 100 && l.battery_status == "Not charging");

        CHECK (l.temps.size () == 6);
        CHECK (l.temps.front ().celsius == 58.0);
        CHECK (l.temps.back ().chip == "nvme");
        bool has_pkg = false;
        for (const Temp &t : l.temps)
            if (t.chip == "coretemp" && t.label == "Package_id_0")
                has_pkg = true;
        CHECK (has_pkg);

        Stats p = parse_stats (check::data ("machines-linux-pi.txt"));
        CHECK (p.hostname == "pi");
        CHECK (p.temps.size () == 1 && p.temps[0].chip == "cpu_thermal");
        CHECK (!p.battery_pct);

        Stats m = parse_stats (check::data ("machines-darwin.txt"));
        CHECK (m.os == "macOS 15.6");
        CHECK (m.ncpu == 8);
        CHECK (m.mem_avail == 6442450000ULL);
        CHECK (m.temps.empty ());
        CHECK (m.battery_status == "discharging");


        bool threw = false;
        try
            {
                parse_stats ("sh: 1: raspdash-stats: not found\n");
            }
        catch (const std::runtime_error &)
            {
                threw = true;
            }
        CHECK (threw);

        CHECK (md5_hex ("") == "d41d8cd98f00b204e9800998ecf8427e");
        CHECK (sha256_hex ("abc") == "ba7816bf8f01cfea414140de5dae2223"
               "b00361a396177a9cb410ff61f20015ad");

        CHECK (keenetic_password ("admin", "Keenetic Starter", "s3cret",
                                  "qERe5XlKNmeUeKxFiTVRJiIUgO10KYNC")
               == "419c1bd3799eb71e639659cf5311dc14"
               "f070951b915a0f7f1283627c6a670690");

        Stats k = parse_keenetic (check::data ("machines-keenetic-system.json"),
                                  check::data ("machines-keenetic-version.json"));
        CHECK (k.hostname == "Keenetic-1234");
        CHECK (k.cpu_pct == 7.0);
        CHECK (k.mem_total == 131072ULL * 1024);
        CHECK (k.mem_avail == (70212ULL + 4096 + 15176) * 1024);
        CHECK (!k.swap_total);
        CHECK (k.uptime_s == 431220.0);
        CHECK (k.os == "Starter · KeeneticOS 4.2.5");

        Stats k2 = parse_keenetic (R"({"cpuload": 3, "memory": "100/400",
                                  "uptime": 60})");
        CHECK (k2.mem_total == 400ULL * 1024 && k2.mem_avail == 300ULL * 1024);
        CHECK (k2.os == "KeeneticOS");

        CommandResult r = run_command ({"/bin/sh", "-s"}, "echo out; echo err >&2;"
        " exit 3\n", 5s);
        CHECK (r.status == 3 && r.out == "out\n" && r.err == "err\n");
        CHECK (!r.timed_out);

        std::string big (1 << 20, '#');
        r = run_command ({"/bin/sh", "-c", "echo agent=1"}, big, 5s);
        CHECK (r.status == 0 && r.out == "agent=1\n");

        auto t0 = steady_clock::now ();
        r = run_command ({"/bin/sh", "-c", "sleep 10"}, "", 300ms);
        CHECK (r.timed_out);
        CHECK (steady_clock::now () - t0 < 3s);

        threw = false;
        try
            {
                run_command ({"/nonexistent/raspdash-no-such-binary"}, "", 1s);
            }
        catch (const std::runtime_error &)
            {
                threw = true;
            }
        CHECK (threw);
    });
}
