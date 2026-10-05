/* test_util.cpp -- tests for util and tui string helpers.

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

#include <clocale>
#include <format>

#include "check.hpp"
#include "tui.hpp"
#include "util.hpp"

using namespace std::chrono;

int
main ()
{

    std::setlocale (LC_ALL, "C.UTF-8");
    return check::run ([]
    {
        auto t = util::parse_iso8601 ("2026-10-05T12:30:00Z");
        CHECK (t.has_value ());
        CHECK (*t == sys_days{2026y / October / 5} + 12h + 30min);
        CHECK (util::parse_iso8601 ("2026-10-05T15:30:00+03:00") == t);
        CHECK (util::parse_iso8601 ("2026-10-05T12:30Z") == t);
        CHECK (util::parse_iso8601 ("2026-10-05T12:30:00.123Z") == t);
        CHECK (util::parse_iso8601 ("2026-10-05")
        == sys_days{2026y / October / 5});
        CHECK (!util::parse_iso8601 ("2026-13-05"));
        CHECK (!util::parse_iso8601 ("garbage"));

        auto base = sys_days{2026y / October / 5};
        CHECK (util::format_age (base, base + 5s) == "5s");
        CHECK (util::format_age (base, base + 12min) == "12m");
        CHECK (util::format_age (base, base + 3h) == "3h");
        CHECK (util::format_age (base, base + 50h) == "2d");

        CHECK (util::human_bytes (512) == "512B");
        CHECK (util::human_bytes (3584) == "3.5K");
        CHECK (util::human_bytes (8ULL << 30) == "8.0G");

        CHECK (util::trim ("  a b \n") == "a b");
        CHECK (util::split ("a,b,,c", ',').size () == 4);

        auto msk = util::to_local (*t, util::zone ("Europe/Moscow"));
        CHECK (std::format ("{:%H:%M}", msk) == "15:30");
        auto jan = sys_days{2026y / January / 15} + 12h;
        auto ath = util::to_local (jan, util::zone ("Europe/Athens"));
        CHECK (std::format ("{:%H:%M}", ath) == "14:00");

        CHECK (tui::width ("abc") == 3);
        CHECK (tui::width ("Привет") == 6);
        CHECK (tui::fit ("Высшая математика", 7) == "Высшая…");
        CHECK (tui::fit ("short", 10) == "short");
        CHECK (tui::pad ("ab", 4, 'r') == "  ab");

        tui::Rect r{0, 0, 10, 20};
        auto cols = r.columns (3, 1);
        CHECK (cols.size () == 3);
        CHECK (cols[0].w == 6 && cols[1].x == 7 && cols[2].right () == 20);
        CHECK (r.inset (1, 2).w == 16);

        tui::Braille b (2, 1);
        CHECK (b.dots_w () == 4 && b.dots_h () == 4);
        b.set (0, 0);
        CHECK (b.used (0, 0) && !b.used (1, 0));
    });
}
