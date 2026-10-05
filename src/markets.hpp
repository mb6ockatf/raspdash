/* markets.hpp -- week-long price charts.

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
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace markets
{
struct Point
{
    util::TimePoint t;
    double v = 0;
};

struct Provider
{
    std::string type;
    std::string symbol;
    std::string vs = "usd";

    std::string label () const;
};

struct Chart
{
    std::string name;
    std::string icon;
    int decimals = -1;
    std::chrono::hours max_age{6};
    std::vector<Provider> providers;
};

struct Result
{
    std::vector<Point> points;
    std::string provider;
    Provider served;
    std::string body;
    std::vector<std::string> failures;
};

std::vector<Point> parse_binance (std::string_view body);
std::vector<Point> parse_bybit (std::string_view body);
std::vector<Point> parse_okx (std::string_view body);
std::vector<Point> parse_coinbase (std::string_view body);
std::vector<Point> parse_kraken (std::string_view body);
std::vector<Point> parse_coingecko (std::string_view body);
std::vector<Point> parse_cbr (std::string_view body);

std::string provider_url (const Provider &p, util::TimePoint now);

std::vector<Point> parse (const Provider &p, std::string_view body);

std::vector<Point> last_week (std::vector<Point> pts);

using Fetcher = std::function<std::string (const Provider &)>;

Result fetch_chart (const Chart &chart, const Fetcher &fetch,
                    util::TimePoint now);

struct Axis
{
    double lo = 0, hi = 1;
    double step = 1;
    int decimals = 0;
    std::vector<double> ticks;
};

Axis nice_axis (double lo, double hi, int max_ticks);

std::string format_price (double v, int decimals);

int auto_decimals (double v);

std::vector<Chart> default_charts ();
}
