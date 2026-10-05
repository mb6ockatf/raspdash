/* test_weather.cpp -- parse saved MET Norway forecasts.

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
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

#include <unistd.h>

#include "check.hpp"
#include "tui.hpp"
#include "weather.hpp"

using namespace std::chrono;
using namespace weather;

namespace
{
bool
near (double a, double b, double eps = 0.01)
{
    return std::fabs (a - b) < eps;
}

const DaySummary *
find_day (const std::vector<DaySummary> &days, year_month_day d)
{
    for (const DaySummary &s : days)
        if (s.date == d)
            return &s;
    return nullptr;
}

Point
point (sys_seconds t, double temp)
{
    Point p;
    p.time = t;
    p.temp = temp;
    return p;
}

template <typename F>
bool
eventually (F cond)
{
    for (int i = 0; i < 100; ++i)
        {
            if (cond ())
                return true;
            std::this_thread::sleep_for (50ms);
        }
    return false;
}

void
restore_test ()
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path ()
                   / std::format ("raspdash-test-weather-{}", ::getpid ());
    fs::create_directories (dir);
    Source::set_cache_dir (dir);
    http::global_init ("raspdash-test");
    const time_zone *msk = locate_zone ("Europe/Moscow");
    const std::string dead = "http://127.0.0.1:9/complete";
    {
        std::ofstream (dir / "weather-59.9386_30.3141.json")
                << check::data ("weather-spb.json");
        std::ofstream (dir / "weather-59.9386_30.3141.json.meta")
                << "Sun, 04 Oct 2026 22:16:57 GMT\nThu, 01 Jan 2099 00:00:00 GMT\n";
    }

    {
        MetSource src (dead, {{"SPb", 59.9386, 30.3141, 11, msk}}, 7200s);
        src.start ();
        bool ok = eventually ([&]
        {
            auto s = src.snapshot.get ();
            return s && s->front ().forecast && src.status ().has_data ();
        });
        CHECK (ok);
        auto s = src.snapshot.get ();
        CHECK (s && s->front ().forecast
               && s->front ().forecast->points.size () == 87);
        std::this_thread::sleep_for (200ms);
        CHECK (src.status ().error.empty ());
        src.stop ();
    }

    {
        MetSource src (dead, {{"SPb", 59.9386, 30.3141, 11, msk},
            {"Moscow", 55.7558, 37.6173, 144, msk}
        }, 7200s);
        src.start ();
        CHECK (eventually ([&]
        {
            return !src.status ().error.empty ();
        }));
        src.stop ();
        auto s = src.snapshot.get ();
        CHECK (s && s->size () == 2 && (*s)[0].forecast
               && (*s)[0].error.empty ());
        CHECK (s && s->size () == 2 && !(*s)[1].forecast
               && !(*s)[1].error.empty ());
    }
    std::error_code ec;
    fs::remove_all (dir, ec);
    Source::set_cache_dir ({});
}
}

int
main ()
{
    std::setlocale (LC_ALL, "C.UTF-8");
    return check::run ([]
    {
        Forecast spb = parse_forecast (check::data ("weather-spb.json"));
        CHECK (spb.points.size () == 87);
        CHECK (spb.updated == sys_days{2026y / October / 4} + 21h + 30min + 15s);
        const Point &p0 = spb.points.front ();
        CHECK (p0.time == sys_days{2026y / October / 4} + 22h);
        CHECK (near (p0.temp, 12.2));
        CHECK (near (p0.feels, 10.6));
        CHECK (near (p0.wind, 4.2) && near (p0.gust, 8.1));
        CHECK (near (p0.wind_dir, 232.0));
        CHECK (near (p0.pressure, 1011.9));
        CHECK (p0.symbol_1h == "rain");
        CHECK (near (p0.precip_1h, 0.7) && near (p0.pop_1h, 37.4));
        CHECK (p0.symbol_6h == "partlycloudy_night");
        CHECK (near (p0.tmin_6h, 11.3) && near (p0.tmax_6h, 12.3));
        const Point &tail = spb.points[spb.points.size () - 2];
        CHECK (tail.symbol_1h.empty () && !has (tail.precip_1h));
        CHECK (!tail.symbol_6h.empty ());

        const time_zone *msk = locate_zone ("Europe/Moscow");
        auto days = daily (spb, msk, 7);
        CHECK (days.size () == 7);
        CHECK (days.front ().date == 2026y / October / 5);
        const DaySummary *d5 = find_day (days, 2026y / October / 5);
        const DaySummary *d6 = find_day (days, 2026y / October / 6);
        CHECK (d5 && near (d5->tmin, 11.3) && near (d5->tmax, 12.6));
        CHECK (d5 && near (d5->precip, 0.7));
        CHECK (d5 && d5->hours == 23);
        CHECK (d6 && near (d6->tmin, 12.1) && near (d6->tmax, 14.5));
        CHECK (d6 && near (d6->precip, 1.9));
        CHECK (d6 && d6->hours == 24 && !d6->symbol.empty ());
        CHECK (daily (spb, msk, 3).size () == 3);
        for (const DaySummary &d : days)
            CHECK (has (d.tmin) && has (d.tmax) && d.tmin <= d.tmax);

        Forecast east = parse_forecast (check::data ("weather-athens-tz.json"));
        auto mdays = daily (east, locate_zone ("Europe/Athens"), 7);
        const DaySummary *m5 = find_day (mdays, 2026y / October / 5);
        CHECK (m5 && near (m5->tmin, 18.1) && near (m5->tmax, 20.5));
        CHECK (m5 && near (m5->precip, 3.4));

        Forecast f;
        Point a = point (sys_days{2026y / October / 10} + 18h, 20);
        a.symbol_6h = "rain";
        a.precip_6h = 6.0;
        a.pop_6h = 80;
        a.tmin_6h = 17;
        a.tmax_6h = 21;
        Point b = point (sys_days{2026y / October / 11} + 0h, 18);
        b.symbol_6h = "cloudy";
        b.precip_6h = 0;
        f.points = {a, b};
        auto sdays = daily (f, locate_zone ("Europe/Athens"), 7);
        CHECK (sdays.size () == 2);
        CHECK (sdays.size () == 2 && near (sdays[0].precip, 3.0)
               && near (sdays[1].precip, 3.0));
        CHECK (sdays.size () == 2 && near (sdays[0].pop, 80));
        CHECK (sdays.size () == 2 && near (sdays[1].tmin, 17)
               && near (sdays[1].tmax, 21));

        tui::set_nerd_icons (false);
        CHECK (symbol ("clearsky_day").text == "Clear");
        CHECK (symbol ("clearsky_night").glyph == "☀");
        CHECK (symbol ("partlycloudy_polartwilight").text == "Partly cloudy");
        CHECK (symbol ("lightrainshowers_day").text == "Light rain showers");
        CHECK (symbol ("heavysnow").text == "Heavy snow");
        CHECK (symbol ("heavysnow").glyph == "❄");
        CHECK (symbol ("sleet").text == "Sleet");
        CHECK (symbol ("rainandthunder").text == "Rain, thunder");
        CHECK (symbol ("rainandthunder").glyph == "↯");
        CHECK (symbol ("lightssleetshowersandthunder_day").text
               == "Light sleet showers, thunder");
        CHECK (symbol ("lightssnowshowersandthunder_night").text
               == "Light snow showers, thunder");
        CHECK (symbol ("lightsnow").text == "Light snow");
        CHECK (symbol ("bogus").glyph == "?");
        CHECK (symbol ("").glyph == "?");
        for (const char *code :
        {
            "clearsky_day", "fair_night", "partlycloudy_day", "cloudy", "fog",
            "lightrain", "rainshowers_day", "sleet", "snow",
            "heavyrainandthunder", "snowshowersandthunder_day"
        })
        {
            tui::set_nerd_icons (false);
            CHECK (tui::width (symbol (code).glyph) == 1);
            tui::set_nerd_icons (true);
            CHECK (tui::width (symbol (code).glyph) == 1);
        }
        tui::set_nerd_icons (true);
        CHECK (symbol ("clearsky_day").glyph == "");
        CHECK (symbol ("clearsky_night").glyph == "");
        CHECK (symbol ("lightrain").glyph == "");

        CHECK (wind_arrow (0) == "\uE340");
        tui::set_nerd_icons (false);
        CHECK (wind_arrow (0) == "↓");
        CHECK (wind_arrow (232) == "↗");
        CHECK (wind_arrow (359) == "↓");
        CHECK (compass (232) == "SW");
        CHECK (compass (350) == "N");

        auto ex = parse_http_date ("Sun, 04 Oct 2026 22:48:15 GMT");
        CHECK (ex && *ex == sys_days{2026y / October / 4} + 22h + 48min + 15s);
        CHECK (!parse_http_date ("yesterday"));

        HttpCache cache;
        CHECK (!cache.fresh (*ex));
        CHECK (cache.conditional_headers ().empty ());
        http::Response r;
        r.status = 200;
        r.headers = {{"last-modified", "Sun, 04 Oct 2026 22:16:57 GMT"},
            {"expires", "Sun, 04 Oct 2026 22:48:15 GMT"}
        };
        cache.update (r);
        CHECK (cache.fresh (*ex - 1s));
        CHECK (!cache.fresh (*ex));
        auto h = cache.conditional_headers ();
        CHECK (h.size () == 1 && h[0].first == "If-Modified-Since"
               && h[0].second == "Sun, 04 Oct 2026 22:16:57 GMT");
        http::Response nm;
        nm.status = 304;
        nm.headers = {{"expires", "Sun, 04 Oct 2026 23:20:00 GMT"}};
        cache.update (nm);
        CHECK (cache.last_modified == "Sun, 04 Oct 2026 22:16:57 GMT");
        CHECK (cache.fresh (*ex + 10min));

        CHECK (forecast_url ("https://x/complete", 59.938631, 30.314131, 11)
               == "https://x/complete?lat=59.9386&lon=30.3141&altitude=11");
        CHECK (forecast_url ("https://x/c", 35.28367, 25.46262, std::nullopt)
               == "https://x/c?lat=35.2836&lon=25.4626");
        restore_test ();

        CHECK (forecast_url ("https://x/c", 35.2837, 25.4626, 28)
               == "https://x/c?lat=35.2837&lon=25.4626&altitude=28");
        CHECK (truncate_coordinate (59.9386) == 59.9386);
        CHECK (forecast_url ("https://x/c", -33.86889, -151.20999, std::nullopt)
               == "https://x/c?lat=-33.8688&lon=-151.2099");
    });
}
