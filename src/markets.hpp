/* markets.hpp -- price charts over a week, a month or half a year.

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

enum class Span
{
    week,
    month,
    half_year
};

struct Range
{
    Span span;
    std::string id;
    std::string label;
    std::chrono::days length;
    std::chrono::hours bucket;
};

const Range &range_of (Span span);

const Range *find_range (std::string_view id);

struct Chart
{
    std::string name;
    std::string icon;
    int decimals = -1;
    std::chrono::hours max_age{6};
    std::vector<Provider> providers;
    std::string screen = "Markets";
    Range range = range_of (Span::week);
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

std::string provider_url (const Provider &p, util::TimePoint now,
                          const Range &range = range_of (Span::week));

std::vector<Point> parse (const Provider &p, std::string_view body);

std::vector<Point> last_span (std::vector<Point> points,
                              std::chrono::days length);

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

struct Statistics
{
    double open = 0;
    double last = 0;
    double high = 0;
    double low = 0;
    double average = 0;
    double change_percent = 0;
    double from_high_percent = 0;
    double from_low_percent = 0;
    double daily_volatility_percent = 0;
};

Statistics statistics (const std::vector<Point> &points);

std::vector<Chart> default_charts ();

std::vector<Chart> expand_ranges (const std::vector<Chart> &instruments,
                                  const std::vector<Range> &ranges);
}
