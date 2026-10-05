/* xmr.hpp -- the XMR screen: Monero market, network, pools and node.

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
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace xmr
{
inline constexpr double atomic_units = 1e12;
inline constexpr double target_block_seconds = 120;
inline constexpr double blocks_per_day = 720;
inline constexpr std::uint64_t tail_emission = 600000000000ULL;

struct Market
{
    std::map<std::string, double> price;
    std::map<std::string, double> ath;
    std::map<std::string, double> ath_change_percent;
    std::optional<util::TimePoint> ath_date;
    double change_24h_percent = 0;
    double change_7d_percent = 0;
    double change_30d_percent = 0;
    double change_60d_percent = 0;
    double change_1y_percent = 0;
    double high_24h_usd = 0;
    double low_24h_usd = 0;
    double market_cap_usd = 0;
    double volume_usd = 0;
    double circulating_supply = 0;
    int rank = 0;
    std::optional<util::TimePoint> updated;
};

Market parse_market (std::string_view body);

struct NetworkInfo
{
    std::uint64_t height = 0;
    std::uint64_t difficulty = 0;
    double hashrate = 0;
    std::uint64_t tx_count = 0;
    int tx_pool_size = 0;
    std::uint64_t tx_pool_bytes = 0;
    std::uint64_t fee_per_kb = 0;
    std::uint64_t block_size_median = 0;
    int hard_fork = 0;
    int incoming_connections = 0;
    int outgoing_connections = 0;
};

NetworkInfo parse_network_info (std::string_view body);

struct Block
{
    std::uint64_t height = 0;
    util::TimePoint timestamp{};
    int transactions = 0;
    std::uint64_t size = 0;
    std::uint64_t fees = 0;
};

std::vector<Block> parse_blocks (std::string_view body);

std::optional<std::chrono::seconds> average_block_time (const std::vector<Block> &blocks);

struct Pool
{
    std::string name;
    double hashrate = 0;
    std::uint64_t difficulty = 0;
    int block_time = 10;
    int active_miners = 0;
    int total_miners = 0;
    int blocks_found = 0;
    std::uint64_t sidechain_height = 0;
    std::uint64_t last_found_height = 0;
    util::TimePoint last_found_at{};
    double effort_current = 0;
    double effort_average = 0;
    std::uint64_t main_height = 0;
    std::uint64_t main_difficulty = 0;
    std::uint64_t main_reward = 0;
    std::string p2pool_version;
    std::string monero_version;
};

Pool parse_pool (std::string_view name, std::string_view body);

struct NodeInfo
{
    std::uint64_t height = 0;
    std::uint64_t target_height = 0;
    std::uint64_t difficulty = 0;
    std::uint64_t tx_count = 0;
    int tx_pool_size = 0;
    std::uint64_t database_size = 0;
    std::optional<std::uint64_t> free_space;
    int incoming_connections = 0;
    int outgoing_connections = 0;
    int rpc_connections = 0;
    int white_peers = 0;
    int grey_peers = 0;
    std::optional<util::TimePoint> start_time;
    std::string version;
    std::string nettype;
    std::string top_block_hash;
    bool synchronized = false;
    bool busy_syncing = false;
    bool restricted = false;
    bool offline = false;
    bool update_available = false;
};

NodeInfo parse_node_info (std::string_view body);

double sync_fraction (const NodeInfo &node);

std::string node_rpc_url (std::string_view node);

std::string format_hashrate (double hashes_per_second);

std::string format_compact (double value);

std::string format_xmr (double amount, int decimals = 4);

std::string format_duration (std::chrono::seconds span);

std::string format_percent (double percent);

double yearly_inflation_percent (double circulating_supply);

struct Estimate
{
    double share = 0;
    double xmr_per_day = 0;
    std::chrono::seconds solo_block_time{};
    double kwh_per_day = 0;
    double power_cost_per_day = 0;
};

Estimate estimate (double hashrate, double network_hashrate, double reward_xmr,
                   double watts, double price_per_kwh);

double pool_shares_per_day (double hashrate, const Pool &pool);
}
