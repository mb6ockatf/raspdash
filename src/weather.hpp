/* weather.hpp -- forecasts from MET Norway's Locationforecast 2.0.

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
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "http.hpp"
#include "source.hpp"
#include "tui.hpp"
#include "util.hpp"

namespace weather
{
inline constexpr double none = std::numeric_limits<double>::quiet_NaN ();

inline bool
has (double v)
{
    return !std::isnan (v);
}

struct Point
{
    util::TimePoint time;

    double temp = none;
    double feels = none;
    double wind = none;
    double gust = none;
    double wind_dir = none;
    double humidity = none;
    double pressure = none;
    double cloud = none;

    std::string symbol_1h;
    double precip_1h = none;
    double pop_1h = none;

    std::string symbol_6h;
    double precip_6h = none;
    double pop_6h = none;
    double tmin_6h = none, tmax_6h = none;
};

struct Forecast
{
    util::TimePoint updated;
    std::vector<Point> points;
};

Forecast parse_forecast (std::string_view json);

struct DaySummary
{
    std::chrono::year_month_day date;
    double tmin = none, tmax = none;
    double precip = 0;
    double pop = none;
    std::string symbol;
    int hours = 0;
};

std::vector<DaySummary> daily (const Forecast &f,
                               const std::chrono::time_zone *tz,
                               std::size_t max_days);

struct Symbol
{
    std::string glyph;
    std::string text;
    tui::Role role;
};

Symbol symbol (std::string_view code);

std::string_view wind_arrow (double deg);
std::string_view compass (double deg);

std::optional<util::TimePoint> parse_http_date (std::string_view s);

struct HttpCache
{
    std::string last_modified;
    std::optional<util::TimePoint> expires;

    bool
    fresh (util::TimePoint now) const
    {
        return expires && now < *expires;
    }

    http::Fields conditional_headers () const;

    void update (const http::Response &r);
};

double truncate_coordinate (double v);

std::string forecast_url (std::string_view base, double lat, double lon,
                          std::optional<int> altitude);
struct City
{
    std::string name;
    double lat = 0, lon = 0;
    std::optional<int> altitude;
    const std::chrono::time_zone *tz = nullptr;
};

struct CityState
{
    City city;
    std::shared_ptr<const Forecast> forecast;
    util::TimePoint fetched{};
    std::string error;
};

class MetSource : public Source
{
public:
    MetSource (std::string base, std::vector<City> cities,
               std::chrono::seconds interval);
    ~MetSource () override;

    Latest<std::vector<CityState>> snapshot;

protected:
    void fetch () override;
    std::optional<util::TimePoint> restore () override;

private:
    std::string key (std::size_t i) const;

    std::string base_;
    std::vector<CityState> state_;
    std::vector<HttpCache> http_;
    std::vector<std::string> raw_;
};
}
