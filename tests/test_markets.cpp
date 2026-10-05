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

        auto week = markets::last_week (cb);
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
               == "https://www.cbr.ru/scripts/XML_dynamic.asp?date_req1=25/09/2026"
               "&date_req2=06/10/2026&VAL_NM_RQ=R01235");
        CHECK (throws ([]
        {
            markets::provider_url ({"yahoo", "X"}, util::Clock::now ());
        }));
        CHECK (markets::default_charts ().size () == 4);
    });
}
