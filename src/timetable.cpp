/* timetable.cpp -- weekly timetable from ruz.spbstu.ru.

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

#include "timetable.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "app.hpp"
#include "http.hpp"
#include "modules.hpp"

using json = nlohmann::json;
using namespace std::chrono;

namespace timetable
{
namespace
{
minutes
parse_hhmm (const std::string &s)
{
    int h = 0, m = 0;
    if (std::sscanf (s.c_str (), "%d:%d", &h, &m) != 2)
        throw std::runtime_error (std::format ("bad time '{}'", s));
    return hours{h} + minutes{m};
}

year_month_day
parse_date (const std::string &s)
{
    int Y = 0;
    unsigned M = 0, D = 0;
    if (std::sscanf (s.c_str (), "%d%*1[-.]%u%*1[-.]%u", &Y, &M, &D) != 3)
        throw std::runtime_error (std::format ("bad date '{}'", s));
    year_month_day ymd{year{Y}, month{M}, day{D}};
    if (!ymd.ok ())
        throw std::runtime_error (std::format ("bad date '{}'", s));
    return ymd;
}

std::string
str (const json &j, const char *key)
{
    auto it = j.find (key);
    return it != j.end () && it->is_string () ? it->get<std::string> () : "";
}
}

Week
parse_week (std::string_view text)
{
    json j = json::parse (text);
    Week w;
    const json &week = j.at ("week");
    w.first = parse_date (week.at ("date_start").get<std::string> ());
    w.last = parse_date (week.at ("date_end").get<std::string> ());
    w.odd = week.value ("is_odd", false);
    if (auto g = j.find ("group"); g != j.end () && g->is_object ())
        w.group = str (*g, "name");

    for (const json &d : j.at ("days"))
        {
            Day day;
            day.date = parse_date (d.at ("date").get<std::string> ());
            for (const json &l : d.at ("lessons"))
                {
                    Lesson les;
                    les.start = parse_hhmm (l.at ("time_start").get<std::string> ());
                    les.end = parse_hhmm (l.at ("time_end").get<std::string> ());
                    les.subject = str (l, "subject");
                    if (auto t = l.find ("typeObj"); t != l.end () && t->is_object ())
                        les.kind = str (*t, "name");
                    if (auto ts = l.find ("teachers"); ts != l.end () && ts->is_array ())
                        for (const json &t : *ts)
                            les.teachers.push_back (str (t, "full_name"));
                    if (auto as = l.find ("auditories");
                            as != l.end () && as->is_array ())
                        for (const json &a : *as)
                            {
                                std::string b;
                                if (auto bl = a.find ("building");
                                        bl != a.end () && bl->is_object ())
                                    b = str (*bl, "abbr");
                                std::string room = str (a, "name");
                                std::string one = b.empty () ? room
                                                  : room.empty () ? b
                                                  : b + " " + room;
                                if (!les.place.empty ())
                                    les.place += ", ";
                                les.place += one;
                            }
                    day.lessons.push_back (std::move (les));
                }
            std::ranges::sort (day.lessons, {}, &Lesson::start);
            w.days.push_back (std::move (day));
        }
    std::ranges::sort (w.days, [] (const Day &a, const Day &b)
    {
        return sys_days{a.date} < sys_days{b.date};
    });
    return w;
}

int
parse_group_id (std::string_view text, std::string_view name)
{
    json j = json::parse (text);
    for (const json &g : j.at ("groups"))
        if (str (g, "name") == name)
            return g.at ("id").get<int> ();
    throw std::runtime_error (std::format ("group '{}' not found on RUZ", name));
}

namespace
{
class RuzSource : public Source
{
public:
    RuzSource (std::string base, std::string group, int group_id,
               seconds interval, const time_zone *tz)
        : Source ("timetable", interval), base_ (std::move (base)),
          group_ (std::move (group)), id_ (group_id), tz_ (tz)
    {
    }

    ~RuzSource () override
    {
        stop ();
    }

    Latest<Week> week;

protected:
    void
    fetch () override
    {
        if (id_ <= 0 && group_.empty ())
            throw std::runtime_error ("set [timetable] group in config.toml");
        if (id_ <= 0)
            id_ = parse_group_id (http::get (std::format ("{}/search/groups?q={}",
                                             base_,
                                             http::url_encode (group_))),
                                  group_);
        year_month_day today = local_today ();
        std::string body = http::get (std::format ("{}/scheduler/{}?date={:%F}",
                                      base_, id_, today));
        week.set (parse_week (body));
        cache_put ("week.json", body);
    }

    std::optional<util::TimePoint>
    restore () override
    {
        auto c = cache_get ("week.json");
        if (!c)
            return std::nullopt;
        Week w = parse_week (c->body);
        if (w.group != group_)
            return std::nullopt;
        week.set (std::move (w));
        return c->saved;
    }

    bool
    expired (util::TimePoint now) const override
    {
        auto w = week.get ();
        if (!w)
            return false;
        year_month_day today{floor<days> (util::to_local (now, tz_))};
        return sys_days{today} > sys_days{w->last};
    }

private:
    year_month_day
    local_today () const
    {
        return year_month_day{floor<days> (util::to_local (util::Clock::now (),
                                           tz_))};
    }

private:
    std::string base_;
    std::string group_;
    int id_;
    const time_zone *tz_;
};

std::string
hhmm (minutes m)
{
    return std::format ("{:02}:{:02}", m.count () / 60, m.count () % 60);
}

class TimetableScreen : public Screen
{
public:
    TimetableScreen (RuzSource &src, const time_zone *tz, bool show_teachers)
        : src_ (src), tz_ (tz), show_teachers_ (show_teachers)
    {
    }

    std::string title () const override
    {
        return "Timetable";
    }
    std::vector<Source *> sources () const override
    {
        return {&src_};
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        auto w = src_.week.get ();
        if (!w)
            {
                tui::put (area.y + 1, area.x + 2, "Loading timetable…",
                          tui::attr (tui::DIM));
                return;
            }
        auto local = util::to_local (now, tz_);
        year_month_day today{floor<days> (local)};
        minutes now_min = duration_cast<minutes> (local - floor<days> (local));

        std::string head = std::format ("{} · {:%d.%m}–{:%d.%m} · {} week",
                                        w->group, w->first, w->last,
                                        w->odd ? "odd" : "even");
        tui::put (area.y, area.x + 1, head, tui::attr (tui::TITLE));
        tui::Rect body = area.below (1);

        if (w->days.empty ())
            {
                tui::put (body.y + 1, body.x + 2, "No classes this week.",
                          tui::attr (tui::GOOD));
                return;
            }

        int ndays = 6;
        for (const Day &d : w->days)
            if (weekday{sys_days{d.date}} == Sunday)
                ndays = 7;
        std::vector<tui::Rect> cols = body.columns (ndays, 0);

        for (int i = 0; i < ndays; ++i)
            {
                year_month_day date{sys_days{w->first} + days{i}};
                const Day *day = nullptr;
                for (const Day &d : w->days)
                    if (d.date == date)
                        day = &d;
                bool is_today = date == today;
                bool is_past = sys_days{date} < sys_days{today};
                std::string label = std::format ("{:%a %d.%m}", sys_days{date});
                attr_t ta = tui::attr (is_today ? tui::ACCENT
                                       : is_past ? tui::DIM : tui::TITLE);
                tui::Rect in = tui::frame (cols[i], label, ta);
                draw_day (in, day, is_today, is_past, now_min);
            }
    }

private:
    void
    draw_day (const tui::Rect &in, const Day *day, bool is_today, bool is_past,
              minutes now_min)
    {
        if (!day || day->lessons.empty ())
            {
                tui::put (in.y, in.x, "—", tui::attr (tui::DIM));
                return;
            }
        int y = in.y;
        bool marked_next = false;
        for (const Lesson &l : day->lessons)
            {
                if (y >= in.bottom ())
                    break;
                bool done = is_past || (is_today && l.end <= now_min);
                bool live = is_today && l.start <= now_min && now_min < l.end;
                bool next = is_today && !marked_next && now_min < l.start;
                if (next)
                    marked_next = true;
                tui::Role time_role = live ? tui::ACCENT : done ? tui::DIM
                                      : next ? tui::GOOD : tui::TITLE;
                tui::Role text_role = done ? tui::DIM : tui::NORMAL;

                std::string mark = live ? "● " : next ? "▸ " : "  ";
                tui::put (y, in.x, mark + hhmm (l.start) + "–" + hhmm (l.end),
                          tui::attr (time_role), in.w);
                ++y;
                y = wrap (y, in, l.subject, tui::attr (text_role)
                          | (done ? 0 : A_BOLD), 2);
                std::string meta = l.kind;
                if (!l.place.empty ())
                    meta += (meta.empty () ? "" : " · ") + l.place;
                if (y < in.bottom ())
                    tui::put (y++, in.x + 2, tui::fit (meta, in.w - 2),
                              tui::attr (tui::DIM));
                if (show_teachers_ && !l.teachers.empty () && y < in.bottom ())
                    tui::put (y++, in.x + 2, tui::fit (l.teachers.front (), in.w - 2),
                              tui::attr (tui::DIM));
                ++y;
            }
    }

    static int
    wrap (int y, const tui::Rect &in, const std::string &text, attr_t a,
          int maxlines)
    {
        int width = in.w - 2;
        std::string line;
        int lines = 0;
        auto flush = [&] (bool last)
        {
            if (y < in.bottom () && !line.empty ())
                tui::put (y++, in.x + 2, last ? tui::fit (line, width) : line, a);
            line.clear ();
            ++lines;
        };
        for (std::string_view word : util::split (text, ' '))
            {
                std::string candidate = line.empty () ? std::string (word)
                                        : line + " " + std::string (word);
                if (tui::width (candidate) <= width || line.empty ())
                    line = std::move (candidate);
                else if (lines + 1 >= maxlines)
                    line = std::move (candidate);
                else
                    {
                        flush (false);
                        line = std::string (word);
                    }
            }
        flush (true);
        return y;
    }

    RuzSource &src_;
    const time_zone *tz_;
    bool show_teachers_;
};
}
}

void
install_timetable (App &app)
{
    using namespace timetable;
    const toml::table &t = app.config ().section ("timetable");
    std::string group = t["group"].value_or<std::string> ("");
    int id = t["group_id"].value_or (0);
    auto interval = seconds (t["interval"].value_or (3 * 86400));
    std::string base = t["api"].value_or<std::string> (
                           "https://ruz.spbstu.ru/api/v1/ruz");
    const time_zone *tz = util::zone (
                              t["timezone"].value_or<std::string> ("Europe/Moscow"));
    bool teachers = t["show_teachers"].value_or (true);

    auto &src = app.add_source (std::make_unique<RuzSource> (
                                    std::move (base), std::move (group), id,
                                    interval, tz));
    app.add_screen (std::make_unique<TimetableScreen> (src, tz, teachers));
}
