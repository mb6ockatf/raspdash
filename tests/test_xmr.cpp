/* test_xmr.cpp -- tests for the XMR screen.

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

#include <cmath>
#include <stdexcept>

#include "check.hpp"
#include "xmr.hpp"

namespace
{
bool
near (double left, double right, double tolerance = 1e-6)
{
    return std::abs (left - right) <= tolerance;
}

bool
throws (auto body)
{
    try
        {
            body ();
        }
    catch (const std::exception &)
        {
            return true;
        }
    return false;
}
}

int
main ()
{
    using namespace std::chrono;
    return check::run ([]
    {
        xmr::Market market = xmr::parse_market (check::data ("xmr-coingecko.json"));
        CHECK (near (market.price.at ("usd"), 555.82));
        CHECK (near (market.price.at ("rub"), 47148));
        CHECK (near (market.price.at ("btc"), 0.00650273));
        CHECK (near (market.change_24h_percent, 1.19637));
        CHECK (near (market.change_7d_percent, 2.23702));
        CHECK (near (market.change_30d_percent, 2.15641));
        CHECK (near (market.change_60d_percent, 52.25221));
        CHECK (near (market.change_1y_percent, 71.66726));
        CHECK (near (market.market_cap_usd, 10455831912));
        CHECK (near (market.volume_usd, 135990407));
        CHECK (near (market.circulating_supply, 18813699.030847073));
        CHECK (near (market.ath.at ("usd"), 797.73));
        CHECK (near (market.ath_change_percent.at ("usd"), -30.32466));
        CHECK (market.ath_date.has_value ());
        CHECK (market.rank == 13);
        CHECK (market.updated == util::TimePoint{seconds{1791224680}});
        CHECK (throws ([]
        {
            xmr::parse_market (R"({"status":{"error_code":429,"error_message":"rate limited"}})");
        }));

        xmr::NetworkInfo info = xmr::parse_network_info (check::data ("xmr-networkinfo.json"));
        CHECK (info.height == 3777601);
        CHECK (info.difficulty == 781778026689ULL);
        CHECK (near (info.hashrate, 6514816889));
        CHECK (near (info.hashrate, static_cast<double> (info.difficulty) / 120, 1));
        CHECK (info.tx_pool_size == 49);
        CHECK (info.tx_pool_bytes == 97769);
        CHECK (info.tx_count == 64427974);
        CHECK (info.hard_fork == 16);
        CHECK (info.block_size_median == 300000);
        CHECK (throws ([]
        {
            xmr::parse_network_info (R"({"status":"fail","message":"down"})");
        }));

        xmr::NodeInfo restricted = xmr::parse_node_info (
            check::data ("xmr-getinfo-restricted.json"));
        CHECK (restricted.height == 3777602);
        CHECK (restricted.target_height == 0);
        CHECK (restricted.synchronized && restricted.restricted);
        CHECK (!restricted.free_space.has_value ());
        CHECK (!restricted.start_time.has_value ());
        CHECK (restricted.version.empty ());
        CHECK (restricted.database_size == 295279001600ULL);
        CHECK (restricted.nettype == "mainnet");
        CHECK (restricted.tx_pool_size == 120);
        CHECK (near (xmr::sync_fraction (restricted), 1.0));

        xmr::NodeInfo syncing = xmr::parse_node_info (
            R"({"jsonrpc":"2.0","id":"0","result":{"status":"OK","height":1000000,)"
            R"("target_height":4000000,"synchronized":false,"busy_syncing":true,)"
            R"("restricted":false,"free_space":500000000000,"start_time":1791000000,)"
            R"("version":"0.18.4.3-release","incoming_connections_count":3,)"
            R"("outgoing_connections_count":12}})");
        CHECK (syncing.busy_syncing && !syncing.synchronized);
        CHECK (near (xmr::sync_fraction (syncing), 0.25));
        CHECK (syncing.free_space == 500000000000ULL);
        CHECK (syncing.start_time == util::TimePoint{seconds{1791000000}});
        CHECK (syncing.version == "0.18.4.3-release");
        CHECK (syncing.outgoing_connections == 12);
        xmr::NodeInfo starting = xmr::parse_node_info (
            R"({"result":{"status":"OK","height":1,"target_height":0,)"
            R"("synchronized":false}})");
        CHECK (!starting.synchronized && starting.target_height == 0);
        CHECK (near (xmr::sync_fraction (starting), 0.0));
        CHECK (throws ([]
        {
            xmr::parse_node_info (R"({"jsonrpc":"2.0","id":"0","error":{"code":-32601,"message":"Method not found"}})");
        }));

        CHECK (xmr::node_rpc_url ("http://192.168.1.10:18081") == "http://192.168.1.10:18081/json_rpc");
        CHECK (xmr::node_rpc_url ("192.168.1.10:18081/") == "http://192.168.1.10:18081/json_rpc");
        CHECK (xmr::node_rpc_url ("https://node.example:18089/json_rpc")
               == "https://node.example:18089/json_rpc");

        CHECK (xmr::format_hashrate (6514816889) == "6.51 GH/s");
        CHECK (xmr::format_hashrate (440040022.7) == "440 MH/s");
        CHECK (xmr::format_hashrate (10000) == "10.0 kH/s");
        CHECK (xmr::format_hashrate (950) == "950 H/s");
        CHECK (xmr::format_compact (10455831912) == "10.46B");
        CHECK (xmr::format_compact (135990407) == "135.99M");
        CHECK (xmr::format_compact (950) == "950");
        CHECK (xmr::format_xmr (0.602036, 4) == "0.6020");
        CHECK (xmr::format_duration (seconds{45}) == "45s");
        CHECK (xmr::format_duration (seconds{119}) == "1m 59s");
        CHECK (xmr::format_duration (hours{27}) == "1d 3h");
        CHECK (xmr::format_duration (days{730}) == "2.0y");
        CHECK (xmr::format_percent (1.19637) == "+1.20%");
        CHECK (near (xmr::yearly_inflation_percent (18813699.030847073),
                     157788.0 / 18813699.030847073 * 100));
        CHECK (xmr::format_percent (-30.32466) == "−30.32%");

        xmr::Estimate result = xmr::estimate (10000, 6514816889, 0.602036, 65, 6.5);
        CHECK (near (result.share, 10000 / 6514816889.0, 1e-15));
        CHECK (near (result.xmr_per_day, 10000 / 6514816889.0 * 720 * 0.602036, 1e-12));
        CHECK (result.solo_block_time == seconds{
            static_cast<long long> (6514816889.0 * 120
                                    / 10000)});
        CHECK (near (result.kwh_per_day, 1.56));
        CHECK (near (result.power_cost_per_day, 10.14));
        CHECK (near (xmr::estimate (0, 1e9, 0.6, 0, 0).xmr_per_day, 0));
    });
}
