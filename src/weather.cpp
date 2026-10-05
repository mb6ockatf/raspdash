/* weather.cpp -- forecasts from MET Norway's Locationforecast 2.0.

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

#include "weather.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "app.hpp"
#include "modules.hpp"

using json = nlohmann::json;
using namespace std::chrono;

namespace weather
{
namespace
{
double
num (const json &obj, const char *key)
{
    auto it = obj.find (key);
    return it != obj.end () && it->is_number () ? it->get<double> () : none;
}

const json *
child (const json &obj, const char *key)
{
    auto it = obj.find (key);
    return it != obj.end () && it->is_object () ? &*it : nullptr;
}

std::string
summary_symbol (const json &period)
{
    if (const json *s = child (period, "summary"))
        if (auto it = s->find ("symbol_code"); it != s->end ()
                && it->is_string ())
            return it->get<std::string> ();
    return {};
}
}

Forecast
parse_forecast (std::string_view text)
{
    json j = json::parse (text);
    const json &props = j.at ("properties");
    Forecast f;
    if (const json *meta = child (props, "meta"))
        if (auto u = meta->find ("updated_at"); u != meta->end ()
                && u->is_string ())
            if (auto t = util::parse_iso8601 (u->get<std::string> ()))
                f.updated = *t;

    for (const json &e : props.at ("timeseries"))
        {
            Point p;
            auto t = util::parse_iso8601 (e.at ("time").get<std::string> ());
            if (!t)
                throw std::runtime_error ("bad time in forecast");
            p.time = *t;
            const json &data = e.at ("data");
            if (const json *inst = child (data, "instant"))
                if (const json *d = child (*inst, "details"))
                    {
                        p.temp = num (*d, "air_temperature");
                        p.feels = num (*d, "apparent_air_temperature");
                        p.wind = num (*d, "wind_speed");
                        p.gust = num (*d, "wind_speed_of_gust");
                        p.wind_dir = num (*d, "wind_from_direction");
                        p.humidity = num (*d, "relative_humidity");
                        p.pressure = num (*d, "air_pressure_at_sea_level");
                        p.cloud = num (*d, "cloud_area_fraction");
                    }
            if (const json *n1 = child (data, "next_1_hours"))
                {
                    p.symbol_1h = summary_symbol (*n1);
                    if (const json *d = child (*n1, "details"))
                        {
                            p.precip_1h = num (*d, "precipitation_amount");
                            p.pop_1h = num (*d, "probability_of_precipitation");
                        }
                }
            if (const json *n6 = child (data, "next_6_hours"))
                {
                    p.symbol_6h = summary_symbol (*n6);
                    if (const json *d = child (*n6, "details"))
                        {
                            p.precip_6h = num (*d, "precipitation_amount");
                            p.pop_6h = num (*d, "probability_of_precipitation");
                            p.tmin_6h = num (*d, "air_temperature_min");
                            p.tmax_6h = num (*d, "air_temperature_max");
                        }
                }
            f.points.push_back (std::move (p));
        }
    std::ranges::sort (f.points, {}, &Point::time);
    return f;
}

namespace
{
struct DayAcc
{
    double tmin = none, tmax = none;
    double precip = 0;
    double pop = none;
    int hours = 0;
    std::vector<std::pair<std::string, double>> symbols;

    void
    temp (double t)
    {
        if (!has (t))
            return;
        tmin = has (tmin) ? std::min (tmin, t) : t;
        tmax = has (tmax) ? std::max (tmax, t) : t;
    }

    void
    weigh (const std::string &sym, double w)
    {
        if (sym.empty ())
            return;
        for (auto &[s, v] : symbols)
            if (s == sym)
                {
                    v += w;
                    return;
                }
        symbols.emplace_back (sym, w);
    }
};
}

std::vector<DaySummary>
daily (const Forecast &f, const time_zone *tz, std::size_t max_days)
{
    std::map<local_days, DayAcc> acc;
    const auto &pts = f.points;
    for (std::size_t i = 0; i < pts.size (); ++i)
        {
            const Point &p = pts[i];
            auto gap = i + 1 < pts.size () ? duration_cast<hours> (pts[i + 1].time
                       - p.time)
                       : hours{6};
            bool hourly = gap <= 1h || p.symbol_6h.empty ();
            int dur = hourly ? 1 : static_cast<int> (std::min (gap, hours{6})
                      .count ());
            if (hourly && p.symbol_1h.empty () && !has (p.precip_1h)
                    && i + 1 == pts.size ())
                continue;
            const std::string &sym = hourly ? p.symbol_1h : p.symbol_6h;
            double precip = hourly ? p.precip_1h : p.precip_6h;
            double pop = hourly ? p.pop_1h : p.pop_6h;
            if (!hourly && has (precip))
                precip *= dur / 6.0;

            auto start = util::to_local (p.time, tz);
            acc[floor<days> (start)].temp (p.temp);
            if (!hourly)
                {
                    auto mid = floor<days> (start + hours{dur} / 2);
                    acc[mid].temp (p.tmin_6h);
                    acc[mid].temp (p.tmax_6h);
                }
            for (int h = 0; h < dur; ++h)
                {
                    auto t = start + hours{h};
                    auto d = floor<days> (t);
                    DayAcc &a = acc[d];
                    ++a.hours;
                    if (has (precip))
                        a.precip += precip / dur;
                    if (has (pop))
                        a.pop = has (a.pop) ? std::max (a.pop, pop) : pop;
                    auto hour = duration_cast<hours> (t - d).count ();
                    a.weigh (sym, hour >= 8 && hour < 20 ? 1.0 : 0.01);
                }
        }

    std::vector<DaySummary> out;
    for (auto &[d, a] : acc)
        {
            if (out.size () >= max_days)
                break;
            if (!out.empty () && a.hours < 6)
                break;
            DaySummary s;
            s.date = year_month_day{d};
            s.tmin = a.tmin;
            s.tmax = a.tmax;
            s.precip = a.precip;
            s.pop = a.pop;
            s.hours = a.hours;
            double best = -1;
            for (const auto &[sym, w] : a.symbols)
                if (w > best)
                    {
                        best = w;
                        s.symbol = sym;
                    }
            out.push_back (std::move (s));
        }
    return out;
}

Symbol
symbol (std::string_view code)
{
    bool night = code.ends_with ("_night");
    for (std::string_view suffix :
            {"_day", "_night", "_polartwilight"
            })
        if (code.ends_with (suffix))
            {
                code.remove_suffix (suffix.size ());
                break;
            }
    if (code == "clearsky")
        return {tui::icon (night ? "" : "", "☀"), "Clear",
                tui::ACCENT};
    if (code == "fair")
        return {tui::icon (night ? "" : "", "☼"), "Fair",
                tui::ACCENT};
    if (code == "partlycloudy")
        return {tui::icon (night ? "" : "", "☁"), "Partly cloudy",
                tui::NORMAL};
    if (code == "cloudy")
        return {tui::icon ("", "☁"), "Cloudy", tui::DIM};
    if (code == "fog")
        return {tui::icon ("", "≡"), "Fog", tui::DIM};

    std::string_view c = code;
    std::string text;
    bool light = false;
    if (c.starts_with ("lightss"))
        {
            c.remove_prefix (6);
            light = true;
        }
    else if (c.starts_with ("light"))
        {
            c.remove_prefix (5);
            light = true;
        }
    else if (c.starts_with ("heavy"))
        {
            c.remove_prefix (5);
            text = "Heavy ";
        }
    if (light)
        text = "Light ";
    bool thunder = c.ends_with ("andthunder");
    if (thunder)
        c.remove_suffix (10);
    bool showers = c.ends_with ("showers");
    if (showers)
        c.remove_suffix (7);

    std::string glyph;
    tui::Role role;
    if (c == "rain")
        {
            glyph = tui::icon (showers ? "" : light ? "" : "",
                               "☂");
            role = tui::SERIES1;
        }
    else if (c == "sleet")
        {
            glyph = tui::icon ("", "☂");
            role = tui::SERIES4;
        }
    else if (c == "snow")
        {
            glyph = tui::icon ("", "❄");
            role = tui::NORMAL;
        }
    else
        return {"?", std::string (code), tui::DIM};

    std::string kind (c);
    if (text.empty ())
        kind[0] = static_cast<char> (kind[0] - 'a' + 'A');
    text += kind;
    if (showers)
        text += " showers";
    if (thunder)
        {
            glyph = tui::icon (showers ? "" : "", "↯");
            role = tui::WARN;
            text += ", thunder";
        }
    return {glyph, text, role};
}

std::string_view
wind_arrow (double deg)
{
    static constexpr std::array<const char *, 8> nerd
    {
        "", "", "", "",
        "", "", "", ""
    };
    static constexpr std::array<const char *, 8> plain
    {"↓", "↙", "←", "↖", "↑", "↗", "→", "↘"};
    if (!has (deg))
        return " ";
    auto i = static_cast<std::size_t> (
                 std::lround (std::fmod (deg, 360.0) / 45.0) % 8);
    return tui::icon (nerd[i], plain[i]);
}

std::string_view
compass (double deg)
{
    static constexpr std::array<std::string_view, 8> names
    {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    if (!has (deg))
        return "?";
    int i = static_cast<int> (std::lround (std::fmod (deg, 360.0) / 45.0)) % 8;
    return names[static_cast<std::size_t> (i)];
}

std::optional<util::TimePoint>
parse_http_date (std::string_view s)
{
    char wday[4], mon[4];
    int d, Y, h, m, sec;
    std::string str (s);
    if (std::sscanf (str.c_str (), "%3s, %d %3s %d %d:%d:%d", wday, &d, mon,
                     &Y, &h, &m, &sec) != 7)
        return std::nullopt;
    static constexpr const char *months[]
        = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
          };
    unsigned M = 0;
    for (unsigned i = 0; i < 12; ++i)
        if (std::strcmp (mon, months[i]) == 0)
            M = i + 1;
    year_month_day ymd{year{Y}, month{M}, day{static_cast<unsigned> (d)}};
    if (!ymd.ok () || h > 23 || m > 59 || sec > 60)
        return std::nullopt;
    return sys_days{ymd} + hours{h} + minutes{m} + seconds{sec};
}

http::Fields
HttpCache::conditional_headers () const
{
    if (last_modified.empty ())
        return {};
    return {{"If-Modified-Since", last_modified}};
}

void
HttpCache::update (const http::Response &r)
{
    if (auto lm = r.header ("last-modified"))
        last_modified = *lm;
    if (auto ex = r.header ("expires"))
        expires = parse_http_date (*ex);
    else
        expires.reset ();
}

double
truncate_coordinate (double v)
{
    return std::trunc (v * 1e4 + (v < 0 ? -1e-7 : 1e-7)) / 1e4;
}

std::string
forecast_url (std::string_view base, double lat, double lon,
              std::optional<int> altitude)
{
    auto trunc4 = truncate_coordinate;
    std::string url = std::format ("{}?lat={:.4f}&lon={:.4f}", base,
                                   trunc4 (lat), trunc4 (lon));
    if (altitude)
        url += std::format ("&altitude={}", *altitude);
    return url;
}

MetSource::MetSource (std::string base, std::vector<City> cities,
                      seconds interval)
    : Source ("weather", interval, seconds{120}), base_ (std::move (base))
{
    for (City &c : cities)
        state_.push_back ({std::move (c), nullptr, {}, {}});
    http_.resize (state_.size ());
    raw_.resize (state_.size ());
    snapshot.set (state_);
}

MetSource::~MetSource ()
{
    stop ();
}

std::string
MetSource::key (std::size_t i) const
{
    const City &c = state_[i].city;
    return std::format ("{:.4f}_{:.4f}.json", truncate_coordinate (c.lat),
                        truncate_coordinate (c.lon));
}

void
MetSource::fetch ()
{
    auto now = util::Clock::now ();
    std::size_t failed = 0;
    std::string first_error;
    for (std::size_t i = 0; i < state_.size (); ++i)
        {
            CityState &st = state_[i];
            HttpCache &cache = http_[i];
            if (st.forecast && cache.fresh (now))
                continue;
            try
                {
                    http::Request req;
                    req.url = forecast_url (base_, st.city.lat, st.city.lon,
                                            st.city.altitude);
                    if (st.forecast)
                        req.headers = cache.conditional_headers ();
                    http::Response r = http::perform (req);
                    if (r.status == 200)
                        {
                            st.forecast = std::make_shared<const Forecast> (
                                              parse_forecast (r.body));
                            raw_[i] = std::move (r.body);
                        }
                    else if (r.status != 304 || !st.forecast)
                        throw http::Error (std::format (
                                               "HTTP {}: {}", r.status,
                                               util::trim (std::string_view (r.body)
                                                           .substr (0, 80))));
                    cache.update (r);
                    if (!raw_[i].empty ())
                        cache_put (key (i), raw_[i]);
                    cache_put (key (i) + ".meta",
                               std::format ("{}\n{}\n", cache.last_modified,
                                            cache.expires
                                            ? std::format ("{:%a, %d %b %Y %T GMT}",
                                                           floor<seconds> (*cache.expires))
                                            : std::string ()));
                    st.fetched = now;
                    st.error.clear ();
                }
            catch (const std::exception &e)
                {
                    st.error = e.what ();
                    ++failed;
                    if (first_error.empty ())
                        first_error = std::format ("{}: {}", st.city.name,
                                                   e.what ());
                }
        }
    snapshot.set (state_);
    if (failed)
        throw std::runtime_error (
            failed == state_.size () ? first_error
            : std::format ("{} of {} cities failed; {}", failed,
                           state_.size (), first_error));
}

std::optional<util::TimePoint>
MetSource::restore ()
{
    std::optional<util::TimePoint> oldest;
    bool all = true;
    for (std::size_t i = 0; i < state_.size (); ++i)
        {
            auto c = cache_get (key (i));
            if (!c)
                {
                    all = false;
                    continue;
                }
            try
                {
                    state_[i].forecast = std::make_shared<const Forecast> (
                                             parse_forecast (c->body));
                }
            catch (const std::exception &)
                {
                    all = false;
                    continue;
                }
            raw_[i] = std::move (c->body);
            state_[i].fetched = c->saved;
            if (!oldest || c->saved < *oldest)
                oldest = c->saved;
            if (auto meta = cache_get (key (i) + ".meta"))
                {
                    auto lines = util::split (meta->body, '\n');
                    if (!lines.empty ())
                        http_[i].last_modified = std::string (lines[0]);
                    if (lines.size () > 1)
                        http_[i].expires = parse_http_date (lines[1]);
                }
        }
    snapshot.set (state_);
    return all ? oldest : std::nullopt;
}

namespace
{
std::string
deg (double t)
{
    if (!has (t))
        return "–";
    return std::format ("{}°", std::lround (t));
}

class WeatherScreen : public Screen
{
public:
    WeatherScreen (MetSource &src, std::size_t days)
        : src_ (src), days_ (days)
    {
    }

    std::string
    title () const override
    {
        return "Weather";
    }
    std::vector<Source *>
    sources () const override
    {
        return {&src_};
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        auto snap = src_.snapshot.get ();
        if (!snap || snap->empty ())
            {
                tui::put (area.y + 1, area.x + 2, "No places configured: add [[weather.cities]] tables to config.toml.",
                          tui::attr (tui::DIM));
                return;
            }
        std::size_t n = snap->size ();
        std::vector<tui::Rect> cells;
        if (area.w >= 90 && n > 1)
            {
                std::size_t nrows = (n + 1) / 2;
                for (const tui::Rect &row : area.rows (static_cast<int> (nrows)))
                    for (const tui::Rect &c : row.columns (2, 0))
                        cells.push_back (c);
            }
        else
            cells = area.rows (static_cast<int> (n));
        for (std::size_t i = 0; i < n && i < cells.size (); ++i)
            draw_city (cells[i], (*snap)[i], now);
    }

private:
    void
    draw_city (const tui::Rect &cell, const CityState &st, util::TimePoint now)
    {
        const time_zone *tz = st.city.tz;
        auto local = util::to_local (now, tz);
        std::string title = std::format ("{}  {:%H:%M}", st.city.name, local);
        tui::Rect in = tui::frame (cell, title);
        if (in.empty ())
            return;
        int y = in.y;
        if (!st.error.empty ())
            tui::put (in.bottom () - 1, in.x, tui::fit ("✗ " + st.error, in.w),
                      tui::attr (tui::BAD));
        if (!st.forecast || st.forecast->points.empty ())
            {
                if (st.error.empty ())
                    tui::put (y, in.x, "Loading…", tui::attr (tui::DIM));
                return;
            }
        const Forecast &f = *st.forecast;
        int bottom = st.error.empty () ? in.bottom () : in.bottom () - 1;

        const Point *cur = &f.points.front ();
        for (const Point &p : f.points)
            if (p.time <= now)
                cur = &p;
        y = draw_now (y, in, *cur);

        auto days = daily (f, tz, days_);
        int avail = bottom - y;
        int min_days = std::min<int> (5, static_cast<int> (days.size ()));
        int max_days = static_cast<int> (days.size ());
        if (avail >= 1 + 4 + 1 + min_days)
            {
                int chart_h = std::clamp (avail - 2 - max_days, 4, 10);
                draw_chart ({y + 1, in.x, chart_h, in.w}, f, tz, now);
                y += chart_h + 1;
            }
        if (y < bottom)
            y = draw_days (y + 1, bottom, in, days, local);
    }

    int
    draw_now (int y, const tui::Rect &in, const Point &p)
    {
        Symbol s = symbol (p.symbol_1h.empty () ? p.symbol_6h : p.symbol_1h);
        int x = in.x;
        x += tui::put (y, x, s.glyph, tui::attr (s.role));
        x += tui::put (y, x, " " + s.text + "  ", tui::attr (tui::NORMAL));
        x += tui::put (y, x, deg (p.temp), tui::attr (tui::ACCENT));
        if (has (p.feels))
            tui::put (y, x, std::format ("  feels {}", deg (p.feels)),
                      tui::attr (tui::DIM), in.right () - x);
        ++y;

        if (y < in.bottom ())
            {
                std::string wind = has (p.wind)
                                   ? std::format ("{} {} {:.0f} m/s", wind_arrow (p.wind_dir),
                                                  compass (p.wind_dir), p.wind)
                                   : std::string ("wind –");
                if (has (p.gust))
                    wind += std::format (", gusts {:.0f}", p.gust);
                std::vector<std::string> extras;
                if (has (p.humidity))
                    extras.push_back (std::format ("{}{:.0f}%",
                                                   tui::icon ("\uE373 ", "RH "), p.humidity));
                if (has (p.pressure))
                    extras.push_back (std::format ("{}{:.0f} hPa",
                                                   tui::icon ("\uE372 ", ""), p.pressure));
                if (has (p.cloud))
                    extras.push_back (std::format ("{}{:.0f}%",
                                                   tui::icon ("\uE33D ", "cloud "), p.cloud));
                int x = in.x + tui::put (y, in.x, wind, tui::attr (tui::NORMAL), in.w);
                for (const std::string &e : extras)
                    {
                        int need = 3 + tui::width (e);
                        if (x + need > in.right ())
                            break;
                        x += tui::put (y, x, "   " + e, tui::attr (tui::DIM));
                    }
                ++y;
            }
        if (y < in.bottom ())
            {
                std::string next;
                if (has (p.precip_1h))
                    next = std::format ("{}Next hour: {:.1f} mm",
                                        tui::icon ("\uE371 ", ""), p.precip_1h);
                if (has (p.pop_1h))
                    next += std::format (" ({:.0f}%)", p.pop_1h);
                tui::put (y, in.x, next.empty () ? "" : next,
                          tui::attr (has (p.precip_1h) && p.precip_1h > 0
                                     ? tui::SERIES1 : tui::DIM), in.w);
                ++y;
            }
        return y;
    }

    void
    draw_chart (const tui::Rect &r, const Forecast &f, const time_zone *tz,
                util::TimePoint now)
    {
        auto from = floor<hours> (now);
        std::vector<const Point *> hrs;
        for (const Point &p : f.points)
            if (p.time >= from && p.time <= from + 24h && has (p.temp))
                hrs.push_back (&p);
        if (hrs.size () < 2)
            return;

        double lo = hrs[0]->temp, hi = lo, pmax = 1.0;
        for (const Point *p : hrs)
            {
                lo = std::min (lo, p->temp);
                hi = std::max (hi, p->temp);
                if (has (p->precip_1h))
                    pmax = std::max (pmax, p->precip_1h);
            }
        if (hi - lo < 2)
            {
                double mid = (hi + lo) / 2;
                lo = mid - 1;
                hi = mid + 1;
            }

        const int label_w = 5;
        int plot_x = r.x + label_w;
        int plot_w = r.w - label_w;
        int plot_h = r.h - 2;
        if (plot_w < 8 || plot_h < 2)
            return;
        tui::put (r.y, r.x, tui::pad (deg (hi), label_w - 1, 'r'),
                  tui::attr (tui::DIM));
        tui::put (r.y + plot_h - 1, r.x, tui::pad (deg (lo), label_w - 1, 'r'),
                  tui::attr (tui::DIM));

        tui::Braille canvas (plot_w, plot_h);
        auto span = duration_cast<minutes> (hrs.back ()->time - hrs.front ()->time)
                    .count ();
        auto xpos = [&] (util::TimePoint t)
        {
            auto m = duration_cast<minutes> (t - hrs.front ()->time).count ();
            return static_cast<int> (std::lround (
                                         static_cast<double> (m) / span
                                         * (canvas.dots_w () - 1)));
        };
        auto ypos = [&] (double t)
        {
            return static_cast<int> (std::lround ((hi - t) / (hi - lo)
                                                  * (canvas.dots_h () - 1)));
        };
        for (std::size_t i = 0; i + 1 < hrs.size (); ++i)
            canvas.line (xpos (hrs[i]->time), ypos (hrs[i]->temp),
                         xpos (hrs[i + 1]->time), ypos (hrs[i + 1]->temp));
        canvas.draw (r.y, plot_x, tui::attr (tui::SERIES2));

        static constexpr std::array<std::string_view, 8> bars
        {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        int py = r.y + plot_h;
        for (int c = 0; c < plot_w; ++c)
            {
                double frac = static_cast<double> (c) / std::max (1, plot_w - 1);
                auto t = hrs.front ()->time
                         + minutes{static_cast<long> (frac * span)};
                const Point *p = hrs.front ();
                for (const Point *q : hrs)
                    if (q->time <= t)
                        p = q;
                if (has (p->precip_1h) && p->precip_1h >= 0.05)
                    {
                        auto i = static_cast<std::size_t> (std::clamp (
                                                               std::lround (p->precip_1h / pmax * 7), 0L, 7L));
                        tui::put (py, plot_x + c, bars[i], tui::attr (tui::SERIES1));
                    }
            }
        tui::put (py, r.x, tui::pad (std::format ("{:.0f}mm", pmax), label_w - 1,
                                     'r'), tui::attr (tui::DIM));

        int step = plot_w >= 48 ? 3 : 6;
        int ly = py + 1;
        int last_end = plot_x - 1;
        for (const Point *p : hrs)
            {
                auto lt = util::to_local (p->time, tz);
                auto hour = duration_cast<hours> (lt - floor<days> (lt)).count ();
                if (hour % step)
                    continue;
                int cx = plot_x + xpos (p->time) / 2;
                std::string label = std::format ("{:02}", hour);
                if (cx <= last_end || cx + 2 > r.right ())
                    continue;
                tui::put (ly, cx, label, tui::attr (hour == 0 ? tui::TITLE
                                                    : tui::DIM));
                last_end = cx + 2;
            }
    }

    int
    draw_days (int y, int bottom, const tui::Rect &in,
               const std::vector<DaySummary> &summaries, local_seconds local)
    {
        year_month_day today{floor<days> (local)};
        bool wide = in.w >= 44;
        for (const DaySummary &d : summaries)
            {
                if (y >= bottom)
                    break;
                std::string label = d.date == today ? "Today"
                                    : std::format ("{:%a %d}", sys_days{d.date});
                Symbol s = symbol (d.symbol);
                int x = in.x;
                x += tui::put (y, x, tui::pad (label, 7),
                               tui::attr (d.date == today ? tui::ACCENT : tui::TITLE));
                x += tui::put (y, x, s.glyph, tui::attr (s.role));
                x += tui::put (y, x, " ");

                std::string temps = std::format ("{:>4}/{:<4}", deg (d.tmin),
                                                 deg (d.tmax));
                std::string rain;
                if (d.precip >= 0.05)
                    rain = std::format ("{:.1f}mm", d.precip);
                if (has (d.pop) && d.pop >= 5)
                    rain += std::format ("{}{:.0f}%", rain.empty () ? "" : " ", d.pop);
                int right_w = 10 + (wide ? 12 : 0);
                int text_w = std::min (in.right () - x - right_w, 22);
                if (text_w > 3)
                    x += tui::put (y, x, tui::pad (s.text, text_w),
                                   tui::attr (tui::NORMAL));
                x += tui::put (y, x, temps, tui::attr (tui::NORMAL));
                if (wide)
                    tui::put (y, x, tui::pad (rain, 12, 'r'),
                              tui::attr (d.precip >= 0.05 ? tui::SERIES1 : tui::DIM));
                ++y;
            }
        return y;
    }

    MetSource &src_;
    std::size_t days_;
};

}
}

void
install_weather (App &app)
{
    using namespace weather;
    const toml::table &t = app.config ().section ("weather");
    auto interval = seconds (t["interval"].value_or (7200));
    std::string base = t["api"].value_or<std::string> (
                           "https://api.met.no/weatherapi/locationforecast/2.0/complete");
    auto days = static_cast<std::size_t> (t["days"].value_or (7));

    std::vector<City> cities;
    if (const toml::array *arr = t["cities"].as_array ())
        for (const toml::node &n : *arr)
            {
                const toml::table *c = n.as_table ();
                if (!c)
                    throw std::runtime_error ("[[weather.cities]] entries must be "
                                              "tables");
                City city;
                city.name = (*c)["name"].value_or<std::string> ("");
                auto lat = (*c)["lat"].value<double> ();
                auto lon = (*c)["lon"].value<double> ();
                if (city.name.empty () || !lat || !lon)
                    throw std::runtime_error ("[[weather.cities]] needs name, lat "
                                              "and lon");
                city.lat = *lat;
                city.lon = *lon;
                if (auto alt = (*c)["altitude"].value<int64_t> ())
                    city.altitude = static_cast<int> (*alt);
                city.tz = util::zone ((*c)["timezone"].value_or<std::string> (""));
                cities.push_back (std::move (city));
            }

    auto &src = app.add_source (std::make_unique<MetSource> (
                                    std::move (base), std::move (cities),
                                    interval));
    app.add_screen (std::make_unique<WeatherScreen> (src, days));
}
