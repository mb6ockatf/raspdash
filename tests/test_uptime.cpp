/* test_uptime.cpp -- tests for the boot history and power-cut detection.

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

#include <algorithm>

#include "check.hpp"
#include "uptime.hpp"
#include "util.hpp"

using namespace std::chrono;
using namespace uptime;

namespace
{
util::TimePoint
at (const char *iso)
{
    return *util::parse_iso8601 (iso);
}

steady_clock::time_point
steady (long seconds_since_start)
{
    return steady_clock::time_point{seconds{1000000 + seconds_since_start}};
}

Observation
observation (util::TimePoint taken, long steady_s, double uptime_s)
{
    return {taken, steady (steady_s), uptime_s};
}

HostHistory
host (const char *kind, bool battery,
      std::initializer_list<util::TimePoint> boots)
{
    HostHistory entry;
    entry.kind = kind;
    entry.battery = battery;
    entry.boots = boots;
    return entry;
}
}

int
main ()
{
    return check::run ([]
    {
        util::TimePoint taken = at ("2026-10-05T17:46:00+03:00");

        HostHistory router;
        CHECK (observe (router, observation (taken, 0, 40080)));
        CHECK (router.boots.size () == 1);
        CHECK (router.boots.front () == at ("2026-10-05T06:38:00+03:00"));
        CHECK (!observe (router, observation (taken + 10s, 10, 40090.4)));
        CHECK (!observe (router, observation (taken + 10s, 10, 40090.4)));
        CHECK (router.boots.size () == 1);

        CHECK (observe (router, observation (taken + 3600s, 3600, 120)));
        CHECK (router.boots.size () == 2);
        CHECK (router.boots.back () == at ("2026-10-05T18:44:00+03:00"));

        HostHistory jumped;
        CHECK (observe (jumped, observation (taken, 0, 600)));
        CHECK (!observe (jumped, observation (taken + 60s, 60, 660)));
        CHECK (observe (jumped, observation (taken + 7260s, 120, 720)));
        CHECK (jumped.boots.size () == 1);
        CHECK (jumped.boots.front () == taken + 7260s - 720s);
        CHECK (!observe (jumped, observation (taken + 7320s, 180, 780)));

        util::TimePoint true_boot = taken - 86400s;
        HostHistory merged;
        merged.boots = {true_boot, true_boot + 5h};
        merged.last = observation (true_boot + 5h + 1000s, 0, 1000);
        CHECK (observe (merged, observation (true_boot + 1010s, 10, 1010)));
        CHECK (merged.boots.size () == 1);
        CHECK (merged.boots.front () == true_boot);

        HostHistory restarted;
        restarted.boots = {taken - 86400s};
        CHECK (observe (restarted, observation (taken, 0, 300)));
        CHECK (restarted.boots.size () == 2);
        CHECK (!observe (restarted, observation (taken + 100s, 0, 470)));
        CHECK (restarted.boots.size () == 2);

        HostHistory ignored;
        CHECK (!observe (ignored, observation (taken, 0, -5)));
        CHECK (!observe (ignored, observation (util::TimePoint{}, 0, 50)));
        CHECK (ignored.boots.empty ());

        HostHistory bounded;
        Settings small;
        small.keep = 3;
        for (int index = 0; index < 6; ++index)
            observe (bounded, observation (taken + hours{index}, 3600 * index, 60), small);
        CHECK (bounded.boots.size () == 3);
        CHECK (bounded.boots.front () == taken + 3h - 60s);

        History today;
        today["router"] = host ("keenetic", false, {at ("2026-10-05T06:38:20+03:00")});
        today["hppavillion"] = host ("ssh", true, {at ("2026-10-05T06:38:50+03:00")});
        today["raspberrypi"] = host ("local", false, {at ("2026-09-20T12:00:00+03:00")});
        std::vector<PowerEvent> events = power_events (today);
        CHECK (events.size () == 1);
        CHECK (events.front ().at == at ("2026-10-05T06:38:20+03:00"));
        CHECK (events.front ().router);
        CHECK (events.front ().boots.size () == 2);
        CHECK (event_hosts (events.front ()) == "router, hppavillion");

        History together;
        together["pi"] = host ("local", false, {at ("2026-10-01T10:05:00Z")});
        together["laptop"] = host ("ssh", false, {at ("2026-10-01T10:00:00Z")});
        together["macbook"] = host ("ssh", true, {at ("2026-10-01T10:20:00Z")});
        events = power_events (together);
        CHECK (events.size () == 1);
        CHECK (events.front ().at == at ("2026-10-01T10:00:00Z"));
        CHECK (event_hosts (events.front ()) == "laptop, pi");
        CHECK (!events.front ().router);

        History alone;
        alone["pi"] = host ("local", false, {at ("2026-10-01T10:00:00Z")});
        alone["laptop"] = host ("ssh", false, {at ("2026-10-01T10:30:00Z")});
        CHECK (power_events (alone).empty ());

        History batteries;
        batteries["macbook"] = host ("ssh", true, {at ("2026-10-01T10:00:00Z")});
        batteries["thinkpad"] = host ("ssh", true, {at ("2026-10-01T10:02:00Z")});
        CHECK (power_events (batteries).empty ());
        batteries["pi"] = host ("local", false, {at ("2026-10-01T10:04:00Z")});
        events = power_events (batteries);
        CHECK (events.size () == 1 && events.front ().boots.size () == 3);

        History several;
        several["router"] = host ("keenetic", false,
        {
            at ("2026-09-01T03:00:00Z"), at ("2026-09-15T03:00:00Z"),
            at ("2026-10-05T03:38:00Z")
        });
        several["pi"] = host ("local", false, {at ("2026-09-15T03:01:00Z")});
        events = power_events (several);
        CHECK (events.size () == 3);
        CHECK (events[0].at == at ("2026-10-05T03:38:00Z"));
        CHECK (events[1].boots.size () == 2);
        CHECK (events[2].boots.size () == 1);

        Settings tight;
        tight.window = 30s;
        CHECK (power_events (together, tight).empty ());

        History restored = history_from_json (history_to_json (several));
        CHECK (restored.size () == 2);
        CHECK (restored["router"].kind == "keenetic");
        CHECK (restored["router"].boots.size () == 3);
        CHECK (restored["pi"].boots.front () == at ("2026-09-15T03:01:00Z"));
        CHECK (history_from_json ("not json").empty ());
        CHECK (history_from_json ("{\"hosts\": {\"x\": 5}}").empty ());

        util::TimePoint now = at ("2026-10-05T18:00:00Z");
        Lane lane;
        lane.boots = {now - 54h, now - 12h};
        lane.cuts = {now - 12h};
        lane.up_now = true;
        lane.last_seen = now;
        std::vector<Cell> cells = timeline (lane, now - 72h, now, 6);
        CHECK (cells.size () == 6);
        CHECK (cells[0] == Cell::unknown);
        CHECK (cells[1] == Cell::boot);
        CHECK (cells[2] == Cell::up);
        CHECK (cells[5] == Cell::cut);

        lane.up_now = false;
        lane.last_seen = now - 30h;
        cells = timeline (lane, now - 72h, now, 6);
        CHECK (cells[3] == Cell::up);
        CHECK (cells[4] == Cell::down);
        CHECK (cells[5] == Cell::cut);
        lane.last_seen = {};
        cells = timeline (lane, now - 72h, now, 6);
        CHECK (cells[4] == Cell::unknown);
        CHECK (std::ranges::all_of (timeline ({}, now - 72h, now, 4), [] (Cell cell)
        {
            return cell == Cell::unknown;
        }));
        CHECK (timeline (lane, now, now - 1h, 4).size () == 4);

        CHECK (format_duration (50880s) == "14h 8m");
        CHECK (format_duration (seconds{3 * 86400 + 4 * 3600 + 120}) == "3d 4h");
        CHECK (format_duration (600s) == "10m");
        CHECK (format_duration (42s) == "42s");
        CHECK (format_duration (-5s) == "0s");

        const time_zone *moscow = util::zone ("Europe/Moscow");
        CHECK (format_when (at ("2026-10-05T03:38:00Z"), moscow) == "Mon 05 Oct 06:38");
    });
}
