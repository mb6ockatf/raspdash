/* test_markets.cpp -- market providers, fallback and chart axes.

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
#include <cmath>
#include <stdexcept>

#include "check.hpp"
#include "markets.hpp"

using namespace std::chrono;
using markets::Point;

namespace
{
util::TimePoint
at (long long unix_seconds)
{
    return util::TimePoint{seconds{unix_seconds}};
}

bool
sorted (const std::vector<Point> &pts)
{
    return std::ranges::is_sorted (pts, {}, &Point::t);
}

bool
near (double a, double b)
{
    return std::abs (a - b) < 1e-6;
}

template <typename F>
bool
throws (F f)
{
    try
        {
            f ();
        }
    catch (const std::runtime_error &)
        {
            return true;
        }
    return false;
}

const util::TimePoint saved_at = at (1791154000);
}

int
main ()
{
    return check::run ([]
    {
        auto kraken = markets::parse_kraken (check::data ("markets-kraken.json"));
        CHECK (kraken.size () == 168 && sorted (kraken));
        CHECK (kraken.back ().t == at (1791151200));
        CHECK (near (kraken.back ().v, 1.538));

        auto cb = markets::parse_coinbase (check::data ("markets-coinbase.json"));
        CHECK (cb.size () == 350 && sorted (cb));
        CHECK (cb.back ().t == at (1791151200) && near (cb.back ().v, 86401.04));

        auto okx = markets::parse_okx (check::data ("markets-okx.json"));
        CHECK (okx.size () == 100 && sorted (okx));
        CHECK (near (okx.back ().v, 86324.6));

        auto bybit = markets::parse_bybit (check::data ("markets-bybit.json"));
        CHECK (bybit.size () == 168 && sorted (bybit));
        CHECK (near (bybit.back ().v, 86324.1));

        auto bin = markets::parse_binance (check::data ("markets-binance.json"));
        CHECK (bin.size () == 168 && sorted (bin));
        CHECK (bin.back ().t == at (1782784800) && near (bin.back ().v, 1.6));

        auto cg = markets::parse_coingecko (check::data ("markets-coingecko.json"));
        CHECK (cg.size () == 169 && sorted (cg));
        CHECK (std::abs (cg.back ().v - 1.5377) < 1e-3);

        auto cbr = markets::parse_cbr (check::data ("markets-cbr.xml"));
        CHECK (cbr.size () >= 4 && sorted (cbr));
        CHECK (cbr.front ().t == sys_days{2026y / September / 29} - 3h);
        CHECK (near (cbr.front ().v, 84.4075));

        CHECK (throws ([]
        {
            markets::parse_bybit (R"({"retCode":10001,"retMsg":"Not supported symbols","result":{}})");
        }));
        CHECK (throws ([]
        {
            markets::parse_okx (R"({"code":"51001","data":[],"msg":"Instrument ID doesn't exist."})");
        }));
        CHECK (throws ([]
        {
            markets::parse_binance (R"({"code":-1121,"msg":"Invalid symbol."})");
        }));
        CHECK (throws ([]
        {
            markets::parse_kraken (R"({"error":["EQuery:Unknown asset pair"]})");
        }));
        CHECK (throws ([]
        {
            markets::parse_cbr ("Too Many Requests");
        }));

        auto week = markets::last_span (cb, days{7});
        CHECK (week.size () == 169);
        CHECK (week.back ().t - week.front ().t == days{7});

        markets::Chart ton{"TON/USDT", "", 3, hours{6},
            {   {"binance", "TONUSDT"}, {"okx", "TON-USDT"},
                {"kraken", "TONUSD"}, {"coingecko", "the-open-network"}
            }};
        std::vector<std::string> asked;
        markets::Fetcher fake = [&asked] (const markets::Provider &p)
        {
            asked.push_back (p.type);
            if (p.type == "okx")
                throw std::runtime_error ("HTTP 404");
            return check::data ("markets-" + p.type + ".json");
        };
        markets::Result r = markets::fetch_chart (ton, fake, saved_at);
        CHECK (r.provider == "kraken TONUSD");
        CHECK (r.served.type == "kraken" && !r.body.empty ());
        CHECK (r.failures.size () == 2);
        CHECK (r.failures.size () == 2
               && r.failures[0].find ("stale") != std::string::npos);
        CHECK ((asked == std::vector<std::string> {"binance", "okx", "kraken"}));

        markets::Chart bad{"X", "", 2, hours{6},
            {{"binance", "TONUSDT"}, {"okx", "TON-USDT"}}};
        bool all_failed = false;
        try
            {
                markets::fetch_chart (bad, fake, saved_at);
            }
        catch (const std::runtime_error &e)
            {
                std::string m = e.what ();
                all_failed = m.find ("binance") != std::string::npos
                             && m.find ("okx") != std::string::npos;
            }
        CHECK (all_failed);

        auto a = markets::nice_axis (84000, 87000, 5);
        CHECK (near (a.step, 1000) && a.decimals == 0);
        CHECK (a.ticks.size () == 4 && near (a.ticks.front (), 84000));
        auto b = markets::nice_axis (1.53, 1.66, 5);
        CHECK (near (b.step, 0.05) && b.decimals == 2);
        CHECK (std::ranges::all_of (b.ticks, [&] (double t)
        {
            return t >= b.lo && t <= b.hi;
        }));
        auto c = markets::nice_axis (83.2, 84.5, 4);
        CHECK (near (c.step, 0.5) && c.decimals == 1);
        CHECK (c.ticks.size () <= 4 && c.ticks.size () >= 2);
        auto flat = markets::nice_axis (5, 5, 4);
        CHECK (flat.hi > flat.lo && !flat.ticks.empty ());
        auto q = markets::nice_axis (100, 110, 5);
        CHECK (near (q.step, 2.5) && q.decimals == 1);

        CHECK (markets::format_price (86401.04, 2) == "86,401.04");
        CHECK (markets::format_price (1234567, 0) == "1,234,567");
        CHECK (markets::format_price (-1234.5, 1) == "-1,234.5");
        CHECK (markets::format_price (999, 0) == "999");
        CHECK (markets::format_price (1.5, 3) == "1.500");

        markets::Provider rub{"cbr", "R01235"};
        CHECK (markets::provider_url (rub, saved_at)
               == "https://www.cbr.ru/scripts/XML_dynamic.asp?date_req1=22/09/2026"
               "&date_req2=06/10/2026&VAL_NM_RQ=R01235");
        CHECK (throws ([]
        {
            markets::provider_url ({"yahoo", "X"}, util::Clock::now ());
        }));
        CHECK (markets::default_charts ().size () == 4);

        const markets::Range &month = markets::range_of (markets::Span::month);
        const markets::Range &half_year
        = markets::range_of (markets::Span::half_year);
        CHECK (markets::find_range ("half-year") == &half_year);
        CHECK (markets::find_range ("year") == nullptr);
        CHECK (markets::provider_url ({"binance", "BTCUSDT"}, saved_at, month)
        .ends_with ("&interval=4h&limit=240"));
        CHECK (markets::provider_url ({"binance", "BTCUSDT"}, saved_at, half_year)
        .ends_with ("&interval=1d&limit=240"));
        CHECK (markets::provider_url ({"coingecko", "bitcoin"}, saved_at, half_year)
        .ends_with ("&days=240"));
        CHECK (markets::provider_url (rub, saved_at, month)
               .starts_with ("https://www.cbr.ru/scripts/XML_dynamic.asp"
                             "?date_req1=23/08/2026&"));

        auto expanded = markets::expand_ranges (markets::default_charts (),
        {month, half_year});
        CHECK (expanded.size () == 8);
        CHECK (expanded[0].name == "XAUT/USDT" && expanded[0].range.id == "month");
        CHECK (expanded[1].name == "XAUT/USDT"
               && expanded[1].range.id == "half-year");
        CHECK (expanded[2].name == "TON/USDT" && expanded[2].screen == "XAUT · TON");
        CHECK (expanded[4].name == "BTC/USD" && expanded[4].screen == "BTC · RUB");
        CHECK (expanded[7].name == "USD/RUB"
               && expanded[7].range.id == "half-year");

        std::vector<markets::Point> daily;
        for (int day_index = 0; day_index < 200; ++day_index)
            daily.push_back ({saved_at - days{day_index}, 1.0});
        std::ranges::reverse (daily);
        CHECK (markets::last_span (daily, days{182}).size () == 183);

        std::vector<markets::Point> rising
        {
            {saved_at - days{2}, 100}, {saved_at - days{1}, 110}, {saved_at, 105}
        };
        markets::Statistics stats = markets::statistics (rising);
        CHECK (near (stats.open, 100) && near (stats.last, 105));
        CHECK (near (stats.high, 110) && near (stats.low, 100));
        CHECK (near (stats.average, 105) && near (stats.change_percent, 5));
        CHECK (stats.from_high_percent < 0 && near (stats.from_low_percent, 5));
        std::vector<markets::Point> hourly;
        for (int hour_index = 0; hour_index <= 48; ++hour_index)
            {
                double price = 100 + hour_index;
                hourly.push_back ({saved_at - hours{48 - hour_index}, price,
                                   price - 0.5, price + 2, price - 1
                                  });
            }
        auto average = markets::moving_average (hourly, hours{24});
        CHECK (average.size () == 25);
        CHECK (near (average.front ().v, 112.5) && near (average.back ().v, 136.5));
        auto day_change = markets::change_over (hourly, hours{24}, hours{1});
        CHECK (day_change && near (*day_change, (148.0 - 124.0) / 124.0 * 100));
        CHECK (!markets::change_over (hourly, hours{72}, hours{1}));
        auto day_candles = markets::make_candles (hourly, std::chrono::minutes{1440},
                           util::zone ("UTC"));
        CHECK (day_candles.size () >= 2 && day_candles.size () <= 3);
        CHECK (near (day_candles.back ().close, 148) && day_candles.back ().high >= 148);
        CHECK (day_candles.front ().low <= day_candles.front ().open);
        CHECK (markets::candle_size (days{30}, 90) == std::chrono::minutes{480});
        CHECK (markets::candle_size (days{182}, 90) == std::chrono::minutes{4320});
        CHECK (markets::format_hours (hours{120}) == "5d");
        CHECK (markets::format_hours (hours{36}) == "36h");

        CHECK (stats.daily_volatility_percent > 9.9
               && stats.daily_volatility_percent < 10.1);
    });
}
