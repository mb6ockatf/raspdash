/* test_timetable.cpp -- parse saved ruz.spbstu.ru responses.

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
#include "timetable.hpp"

using namespace std::chrono;

int
main ()
{
    return check::run ([]
    {
        std::string groups = check::data ("ruz-groups.json");
        CHECK (timetable::parse_group_id (groups, "5151003/50001") == 45516);
        CHECK (timetable::parse_group_id (groups, "5151003/50001_2025")
               == 42716);
        bool threw = false;
        try
            {
                timetable::parse_group_id (groups, "5151003/5000");
            }
        catch (const std::runtime_error &)
            {
                threw = true;
            }
        CHECK (threw);

        timetable::Week w = timetable::parse_week (check::data ("ruz-week.json"));
        CHECK (w.group == "5151003/50001");
        CHECK (w.first == 2026y / October / 5);
        CHECK (w.last == 2026y / October / 11);
        CHECK (!w.odd);
        CHECK (w.days.size () == 6);

        const timetable::Day &mon = w.days.front ();
        CHECK (mon.date == 2026y / October / 5);
        CHECK (mon.lessons.size () == 3);
        const timetable::Lesson &l = mon.lessons.front ();
        CHECK (l.start == 9h);
        CHECK (l.end == 11h + 40min);
        CHECK (l.subject == "Высшая математика");
        CHECK (l.kind == "Лекции");
        CHECK (l.place == "ГЗ 215");
        CHECK (l.teachers.size () == 1
               && l.teachers[0] == "Корнилов Максим Вячеславович");

        const timetable::Day &wed = w.days[2];
        CHECK (wed.lessons.front ().teachers.empty ());
        CHECK (wed.lessons.front ().place == "СК Секции");
    });
}
