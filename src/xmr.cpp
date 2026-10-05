/* xmr.cpp -- the XMR screen: Monero market, network, pools and node.

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

#include "xmr.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "app.hpp"
#include "http.hpp"
#include "markets.hpp"
#include "modules.hpp"

using json = nlohmann::json;
using namespace std::chrono;

namespace xmr
{
namespace
{
double
number_of (const json &node, double fallback = 0)
{
    if (node.is_number ())
        return node.get<double> ();
    if (node.is_string ())
        {
            try
                {
                    return std::stod (node.get<std::string> ());
                }
            catch (const std::exception &)
                {
                    return fallback;
                }
        }
    return fallback;
}

std::uint64_t
unsigned_of (const json &node)
{
    if (node.is_number_unsigned ())
        return node.get<std::uint64_t> ();
    if (node.is_number_integer ())
        return static_cast<std::uint64_t> (std::max<std::int64_t> (0, node.get<std::int64_t> ()));
    if (node.is_number_float ())
        return static_cast<std::uint64_t> (std::max (0.0, node.get<double> ()));
    if (node.is_string ())
        {
            std::string text = node.get<std::string> ();
            try
                {
                    return text.starts_with ("0x") ? std::stoull (text.substr (2), nullptr, 16)
                           : std::stoull (text);
                }
            catch (const std::exception &)
                {
                    return 0;
                }
        }
    return 0;
}

const json &
member (const json &object, std::string_view key)
{
    static const json missing;
    if (!object.is_object ())
        return missing;
    auto found = object.find (key);
    return found == object.end () ? missing : *found;
}

double
number_at (const json &object, std::string_view key, double fallback = 0)
{
    return number_of (member (object, key), fallback);
}

std::uint64_t
unsigned_at (const json &object, std::string_view key)
{
    return unsigned_of (member (object, key));
}

int
int_at (const json &object, std::string_view key)
{
    return static_cast<int> (unsigned_at (object, key));
}

std::string
string_at (const json &object, std::string_view key)
{
    const json &node = member (object, key);
    return node.is_string () ? node.get<std::string> () : std::string{};
}

bool
bool_at (const json &object, std::string_view key)
{
    const json &node = member (object, key);
    return node.is_boolean () && node.get<bool> ();
}

std::map<std::string, double>
currency_map (const json &object)
{
    std::map<std::string, double> values;
    if (object.is_object ())
        for (const auto &[currency, value] : object.items ())
            if (value.is_number ())
                values[currency] = value.get<double> ();
    return values;
}

util::TimePoint
from_unix (std::uint64_t seconds_since_epoch)
{
    return util::TimePoint{seconds{static_cast<long long> (seconds_since_epoch)}};
}
}

Market
parse_market (std::string_view body)
{
    json document = json::parse (body);
    if (const json &status = member (document, "status"); status.is_object ())
        throw std::runtime_error (std::format ("coingecko: {}",
                                               string_at (status, "error_message")));
    if (const json &error = member (document, "error"); !error.is_null ())
        throw std::runtime_error (std::format ("coingecko: {}", error.is_string ()
                                               ? error.get<std::string> () : error.dump ()));
    const json &data = document.at ("market_data");
    Market market;
    market.price = currency_map (member (data, "current_price"));
    if (!market.price.contains ("usd"))
        throw std::runtime_error ("coingecko: no USD price");
    market.ath = currency_map (member (data, "ath"));
    market.ath_change_percent = currency_map (member (data, "ath_change_percentage"));
    std::string ath_date = string_at (member (data, "ath_date"), "usd");
    if (!ath_date.empty ())
        market.ath_date = util::parse_iso8601 (ath_date);
    market.change_24h_percent = number_at (data, "price_change_percentage_24h");
    market.change_7d_percent = number_at (data, "price_change_percentage_7d");
    market.change_30d_percent = number_at (data, "price_change_percentage_30d");
    market.change_60d_percent = number_at (data, "price_change_percentage_60d");
    market.change_1y_percent = number_at (data, "price_change_percentage_1y");
    market.high_24h_usd = number_at (member (data, "high_24h"), "usd");
    market.low_24h_usd = number_at (member (data, "low_24h"), "usd");
    market.market_cap_usd = number_at (member (data, "market_cap"), "usd");
    market.volume_usd = number_at (member (data, "total_volume"), "usd");
    market.circulating_supply = number_at (data, "circulating_supply");
    market.rank = static_cast<int> (number_at (document, "market_cap_rank"));
    std::string updated = string_at (document, "last_updated");
    if (!updated.empty ())
        market.updated = util::parse_iso8601 (updated);
    return market;
}

NetworkInfo
parse_network_info (std::string_view body)
{
    json document = json::parse (body);
    if (string_at (document, "status") != "success")
        throw std::runtime_error (std::format ("explorer: {}",
                                               string_at (document, "message").empty ()
                                               ? std::string ("request failed")
                                               : string_at (document, "message")));
    const json &data = document.at ("data");
    NetworkInfo info;
    info.height = unsigned_at (data, "height");
    info.difficulty = unsigned_at (data, "difficulty");
    info.hashrate = number_at (data, "hash_rate",
                               static_cast<double> (info.difficulty) / target_block_seconds);
    info.tx_count = unsigned_at (data, "tx_count");
    info.tx_pool_size = int_at (data, "tx_pool_size");
    info.tx_pool_bytes = unsigned_at (data, "tx_pool_size_kbytes");
    info.fee_per_kb = unsigned_at (data, "fee_per_kb");
    info.block_size_median = unsigned_at (data, "block_size_median");
    info.hard_fork = int_at (data, "current_hf_version");
    info.incoming_connections = int_at (data, "incoming_connections_count");
    info.outgoing_connections = int_at (data, "outgoing_connections_count");
    if (info.height == 0)
        throw std::runtime_error ("explorer: no block height");
    return info;
}

std::vector<Block>
parse_blocks (std::string_view body)
{
    json document = json::parse (body);
    if (string_at (document, "status") != "success")
        throw std::runtime_error ("explorer: block list request failed");
    std::vector<Block> blocks;
    for (const json &entry : document.at ("data").at ("blocks"))
        {
            Block block;
            block.height = unsigned_at (entry, "height");
            block.timestamp = from_unix (unsigned_at (entry, "timestamp"));
            block.size = unsigned_at (entry, "size");
            for (const json &transaction : member (entry, "txs"))
                {
                    if (bool_at (transaction, "coinbase"))
                        continue;
                    ++block.transactions;
                    block.fees += unsigned_at (transaction, "tx_fee");
                }
            blocks.push_back (block);
        }
    std::ranges::sort (blocks, std::ranges::greater{}, &Block::height);
    return blocks;
}

std::optional<seconds>
average_block_time (const std::vector<Block> &blocks)
{
    if (blocks.size () < 2)
        return std::nullopt;
    auto [oldest, newest] = std::ranges::minmax (blocks | std::views::transform (
                                &Block::timestamp));
    auto span = duration_cast<seconds> (newest - oldest);
    return seconds{span.count () / static_cast<long long> (blocks.size () - 1)};
}

Pool
parse_pool (std::string_view name, std::string_view body)
{
    json document = json::parse (body);
    const json &sidechain = member (document, "sidechain");
    if (!sidechain.is_object ())
        throw std::runtime_error (std::format ("p2pool {}: no sidechain data", name));
    Pool pool;
    pool.name = name;
    pool.difficulty = unsigned_at (sidechain, "difficulty");
    pool.block_time = std::max (1, int_at (sidechain, "block_time"));
    pool.hashrate = static_cast<double> (pool.difficulty) / pool.block_time;
    pool.active_miners = int_at (member (sidechain, "window"), "miners");
    pool.total_miners = int_at (sidechain, "miners");
    pool.blocks_found = int_at (sidechain, "found");
    pool.sidechain_height = unsigned_at (sidechain, "height");
    const json &found = member (member (sidechain, "last_found"), "main_block");
    pool.last_found_height = unsigned_at (found, "height");
    pool.last_found_at = from_unix (unsigned_at (found, "timestamp"));
    const json &effort = member (sidechain, "effort");
    pool.effort_current = number_at (effort, "current");
    pool.effort_average = number_at (effort, "average");
    const json &mainchain = member (document, "mainchain");
    pool.main_height = unsigned_at (mainchain, "height");
    pool.main_difficulty = unsigned_at (mainchain, "difficulty");
    pool.main_reward = unsigned_at (mainchain, "reward");
    const json &versions = member (document, "versions");
    pool.p2pool_version = string_at (member (versions, "p2pool"), "version");
    pool.monero_version = string_at (member (versions, "monero"), "version");
    return pool;
}

NodeInfo
parse_node_info (std::string_view body)
{
    json document = json::parse (body);
    if (const json &error = member (document, "error"); error.is_object ())
        throw std::runtime_error (std::format ("monerod: {}",
                                               string_at (error, "message")));
    const json &result = document.at ("result");
    std::string status = string_at (result, "status");
    if (!status.empty () && status != "OK")
        throw std::runtime_error (std::format ("monerod: {}", status));
    NodeInfo node;
    node.height = unsigned_at (result, "height");
    node.target_height = unsigned_at (result, "target_height");
    node.difficulty = unsigned_at (result, "difficulty");
    node.tx_count = unsigned_at (result, "tx_count");
    node.tx_pool_size = int_at (result, "tx_pool_size");
    node.database_size = unsigned_at (result, "database_size");
    std::uint64_t free_space = unsigned_at (result, "free_space");
    if (free_space != 0 && free_space != std::numeric_limits<std::uint64_t>::max ())
        node.free_space = free_space;
    node.incoming_connections = int_at (result, "incoming_connections_count");
    node.outgoing_connections = int_at (result, "outgoing_connections_count");
    node.rpc_connections = int_at (result, "rpc_connections_count");
    node.white_peers = int_at (result, "white_peerlist_size");
    node.grey_peers = int_at (result, "grey_peerlist_size");
    if (std::uint64_t started = unsigned_at (result, "start_time"))
        node.start_time = from_unix (started);
    node.version = string_at (result, "version");
    node.nettype = string_at (result, "nettype");
    node.top_block_hash = string_at (result, "top_block_hash");
    node.synchronized = bool_at (result, "synchronized");
    node.busy_syncing = bool_at (result, "busy_syncing");
    node.restricted = bool_at (result, "restricted");
    node.offline = bool_at (result, "offline");
    node.update_available = bool_at (result, "update_available");
    return node;
}

double
sync_fraction (const NodeInfo &node)
{
    if (node.height == 0)
        return 0;
    if (node.target_height <= node.height)
        return node.synchronized || node.target_height != 0 ? 1.0 : 0.0;
    return static_cast<double> (node.height) / static_cast<double> (node.target_height);
}

std::string
node_rpc_url (std::string_view node)
{
    std::string url (util::trim (node));
    while (url.ends_with ('/'))
        url.pop_back ();
    if (url.find ("://") == std::string::npos)
        url = "http://" + url;
    if (!url.ends_with ("/json_rpc"))
        url += "/json_rpc";
    return url;
}

std::string
format_hashrate (double hashes_per_second)
{
    static constexpr const char *units[] = {"H/s", "kH/s", "MH/s", "GH/s", "TH/s", "PH/s"};
    std::size_t unit = 0;
    double value = std::max (0.0, hashes_per_second);
    while (value >= 1000 && unit + 1 < std::size (units))
        {
            value /= 1000;
            ++unit;
        }
    int decimals = value < 10 ? 2 : value < 100 ? 1 : 0;
    if (unit == 0)
        decimals = 0;
    return std::format ("{:.{}f} {}", value, decimals, units[unit]);
}

std::string
format_compact (double value)
{
    static constexpr const char *suffixes[] = {"", "K", "M", "B", "T"};
    std::size_t suffix = 0;
    double magnitude = std::abs (value);
    while (magnitude >= 1000 && suffix + 1 < std::size (suffixes))
        {
            magnitude /= 1000;
            ++suffix;
        }
    if (suffix == 0)
        return markets::format_price (value, 0);
    return std::format ("{}{:.2f}{}", value < 0 ? "-" : "", magnitude, suffixes[suffix]);
}

std::string
format_xmr (double amount, int decimals)
{
    return std::format ("{:.{}f}", amount, std::max (0, decimals));
}

std::string
format_duration (seconds span)
{
    long long total = std::max<long long> (0, span.count ());
    if (total >= 365LL * 86400)
        return std::format ("{:.1f}y", static_cast<double> (total) / (365.25 * 86400));
    if (total >= 86400)
        return std::format ("{}d {}h", total / 86400, total % 86400 / 3600);
    if (total >= 3600)
        return std::format ("{}h {}m", total / 3600, total % 3600 / 60);
    if (total >= 60)
        return std::format ("{}m {}s", total / 60, total % 60);
    return std::format ("{}s", total);
}

std::string
format_percent (double percent)
{
    return std::format ("{}{:.2f}%", percent < 0 ? "−" : "+", std::abs (percent));
}

double
yearly_inflation_percent (double circulating_supply)
{
    if (circulating_supply <= 0)
        return 0;
    return static_cast<double> (tail_emission) / atomic_units * blocks_per_day * 365.25
           / circulating_supply * 100;
}

Estimate
estimate (double hashrate, double network_hashrate, double reward_xmr, double watts,
          double price_per_kwh)
{
    Estimate result;
    if (network_hashrate > 0)
        result.share = hashrate / network_hashrate;
    result.xmr_per_day = result.share * blocks_per_day * reward_xmr;
    if (hashrate > 0)
        result.solo_block_time = seconds{static_cast<long long> (
                                             network_hashrate * target_block_seconds / hashrate)};
    result.kwh_per_day = watts * 24 / 1000;
    result.power_cost_per_day = result.kwh_per_day * price_per_kwh;
    return result;
}

double
pool_shares_per_day (double hashrate, const Pool &pool)
{
    if (pool.difficulty == 0)
        return 0;
    return hashrate * 86400 / static_cast<double> (pool.difficulty);
}

namespace
{
template <typename T>
struct Stamped
{
    T value;
    util::TimePoint fetched{};
};

struct Chart
{
    std::vector<markets::Point> points;
    std::string provider;
};

constexpr std::string_view coingecko_coin
    = "https://api.coingecko.com/api/v3/coins/monero?localization=false"
      "&tickers=false&market_data=true&community_data=false&developer_data=false"
      "&sparkline=false";
constexpr std::string_view kraken_chart
    = "https://api.kraken.com/0/public/OHLC?pair=XMRUSD&interval=240";
constexpr std::string_view coingecko_chart
    = "https://api.coingecko.com/api/v3/coins/monero/market_chart"
      "?vs_currency=usd&days=30";
constexpr std::string_view explorer_default = "https://xmrchain.net";
constexpr days chart_span{30};

class MarketSource : public Source
{
public:
    explicit MarketSource (seconds interval)
        : Source ("xmr-market", interval)
    {
    }

    ~MarketSource () override
    {
        stop ();
    }

    Latest<Stamped<Market>> market;

protected:
    void
    fetch () override
    {
        std::string body = http::get (std::string (coingecko_coin));
        Market parsed = parse_market (body);
        cache_put ("coingecko.json", body);
        market.set ({std::move (parsed), util::Clock::now ()});
    }

    std::optional<util::TimePoint>
    restore () override
    {
        auto cached = cache_get ("coingecko.json");
        if (!cached)
            return std::nullopt;
        market.set ({parse_market (cached->body), cached->saved});
        return cached->saved;
    }
};

Chart
chart_from (std::string_view provider, std::string_view body)
{
    Chart chart;
    chart.provider = provider;
    chart.points = markets::last_span (provider == "Kraken" ? markets::parse_kraken (body)
                                       : markets::parse_coingecko (body), chart_span);
    if (chart.points.size () < 2)
        throw std::runtime_error (std::format ("{}: too few prices", provider));
    return chart;
}

class ChartSource : public Source
{
public:
    explicit ChartSource (seconds interval)
        : Source ("xmr-chart", interval)
    {
    }

    ~ChartSource () override
    {
        stop ();
    }

    Latest<Stamped<Chart>> chart;

protected:
    void
    fetch () override
    {
        std::string failures;
        for (auto [provider, url] :
                {
                    std::pair{std::string_view ("Kraken"), kraken_chart},
                    std::pair{std::string_view ("CoinGecko"), coingecko_chart}
                })
            try
                {
                    std::string body = http::get (std::string (url));
                    Chart parsed = chart_from (provider, body);
                    json wrapped = {{"provider", provider}, {"body", body}};
                    cache_put ("chart.json", wrapped.dump ());
                    chart.set ({std::move (parsed), util::Clock::now ()});
                    return;
                }
            catch (const std::exception &error)
                {
                    failures += (failures.empty () ? "" : "; ") + std::string (error.what ());
                }
        throw std::runtime_error (failures);
    }

    std::optional<util::TimePoint>
    restore () override
    {
        auto cached = cache_get ("chart.json");
        if (!cached)
            return std::nullopt;
        json wrapped = json::parse (cached->body);
        chart.set ({chart_from (wrapped.at ("provider").get<std::string> (),
                                wrapped.at ("body").get<std::string> ()),
                    cached->saved
                   });
        return cached->saved;
    }
};

class ChainSource : public Source
{
public:
    ChainSource (std::string explorer, seconds interval)
        : Source ("xmr-network", interval), explorer_ (std::move (explorer))
    {
    }

    ~ChainSource () override
    {
        stop ();
    }

    Latest<Stamped<NetworkInfo>> info;
    Latest<Stamped<std::vector<Block>>> blocks;

protected:
    void
    fetch () override
    {
        std::string info_body = http::get (explorer_ + "/api/networkinfo");
        NetworkInfo parsed_info = parse_network_info (info_body);
        cache_put ("networkinfo.json", info_body);
        info.set ({std::move (parsed_info), util::Clock::now ()});

        std::string blocks_body = http::get (explorer_ + "/api/transactions?limit=15");
        std::vector<Block> parsed_blocks = parse_blocks (blocks_body);
        cache_put ("blocks.json", blocks_body);
        blocks.set ({std::move (parsed_blocks), util::Clock::now ()});
    }

    std::optional<util::TimePoint>
    restore () override
    {
        std::optional<util::TimePoint> restored;
        if (auto cached = cache_get ("networkinfo.json"))
            {
                info.set ({parse_network_info (cached->body), cached->saved});
                restored = cached->saved;
            }
        if (auto cached = cache_get ("blocks.json"))
            {
                blocks.set ({parse_blocks (cached->body), cached->saved});
                restored = restored ? std::min (*restored, cached->saved) : cached->saved;
            }
        return restored;
    }

private:
    std::string explorer_;
};

std::string
pool_url (std::string_view name)
{
    if (name == "main")
        return "https://p2pool.observer/api/pool_info";
    return std::format ("https://{}.p2pool.observer/api/pool_info", name);
}

class PoolsSource : public Source
{
public:
    PoolsSource (std::vector<std::string> names, seconds interval)
        : Source ("xmr-pools", interval), names_ (std::move (names))
    {
    }

    ~PoolsSource () override
    {
        stop ();
    }

    const std::vector<std::string> &
    names () const
    {
        return names_;
    }

    Latest<std::vector<std::optional<Stamped<Pool>>>> pools;

protected:
    void
    fetch () override
    {
        auto previous = pools.get ();
        std::vector<std::optional<Stamped<Pool>>> next (names_.size ());
        if (previous)
            next = *previous;
        std::string failures;
        for (std::size_t index = 0; index < names_.size (); ++index)
            try
                {
                    std::string body = http::get (pool_url (names_[index]));
                    Pool parsed = parse_pool (names_[index], body);
                    cache_put ("pool-" + names_[index] + ".json", body);
                    next[index] = Stamped<Pool> {std::move (parsed), util::Clock::now ()};
                }
            catch (const std::exception &error)
                {
                    failures += std::format ("{}{}: {}", failures.empty () ? "" : "; ",
                                             names_[index], error.what ());
                }
        pools.set (std::move (next));
        if (!failures.empty ())
            throw std::runtime_error (failures);
    }

    std::optional<util::TimePoint>
    restore () override
    {
        std::vector<std::optional<Stamped<Pool>>> restored (names_.size ());
        std::optional<util::TimePoint> oldest;
        for (std::size_t index = 0; index < names_.size (); ++index)
            if (auto cached = cache_get ("pool-" + names_[index] + ".json"))
                {
                    try
                        {
                            restored[index] = Stamped<Pool> {parse_pool (names_[index], cached->body),
                                                             cached->saved
                                                            };
                            oldest = oldest ? std::min (*oldest, cached->saved) : cached->saved;
                        }
                    catch (const std::exception &)
                        {
                        }
                }
        pools.set (std::move (restored));
        return oldest;
    }

private:
    std::vector<std::string> names_;
};

struct NodeState
{
    NodeInfo info;
    util::TimePoint fetched{};
    std::optional<double> blocks_per_second;
};

class NodeSource : public Source
{
public:
    NodeSource (std::string url, bool direct, seconds interval)
        : Source ("xmr-node", interval), url_ (std::move (url)), direct_ (direct)
    {
    }

    ~NodeSource () override
    {
        stop ();
    }

    std::string
    address () const
    {
        std::string shown = url_;
        if (shown.ends_with ("/json_rpc"))
            shown.resize (shown.size () - 9);
        return shown;
    }

    Latest<NodeState> node;

protected:
    void
    fetch () override
    {
        http::Request request{url_, "POST", {{"Content-Type", "application/json"}},
            R"({"jsonrpc":"2.0","id":"0","method":"get_info"})"
        };
        request.timeout = seconds{10};
        request.direct = direct_;
        http::Response response = http::perform (request);
        if (!response.ok ())
            throw std::runtime_error (std::format ("HTTP {} from {}", response.status, url_));
        NodeState next;
        next.info = parse_node_info (response.body);
        next.fetched = util::Clock::now ();
        if (auto previous = node.get ())
            {
                double elapsed = duration<double> (next.fetched - previous->fetched).count ();
                if (elapsed > 0 && next.info.height >= previous->info.height
                        && previous->info.height > 0)
                    {
                        double rate = static_cast<double> (next.info.height
                                                           - previous->info.height) / elapsed;
                        next.blocks_per_second = previous->blocks_per_second
                                                 ? 0.7 * *previous->blocks_per_second + 0.3 * rate : rate;
                    }
            }
        cache_put ("get_info.json", response.body);
        node.set (std::move (next));
    }

    std::optional<util::TimePoint>
    restore () override
    {
        auto cached = cache_get ("get_info.json");
        if (!cached)
            return std::nullopt;
        node.set ({parse_node_info (cached->body), cached->saved, std::nullopt});
        return cached->saved;
    }

private:
    std::string url_;
    bool direct_ = true;
};

struct Settings
{
    double hashrate = 10000;
    double watts = 65;
    double price_per_kwh = 6.5;
    std::string currency = "rub";
    std::string timezone;
};

using Segments = std::vector<std::pair<std::string, attr_t>>;

int
put_segments (int row, int column, int right_edge, const Segments &segments)
{
    int start = column;
    for (std::size_t index = 0; index < segments.size (); ++index)
        {
            const auto &[text, attributes] = segments[index];
            int room = right_edge - column;
            if (room <= 0)
                break;
            int needed = tui::width (text);
            if (text == " · " && index + 1 < segments.size ())
                needed += tui::width (segments[index + 1].first);
            if (index > 0 && needed > room)
                break;
            column += tui::put (row, column, tui::fit (text, room), attributes, room);
        }
    return column - start;
}

class Lines
{
public:
    Lines (const tui::Rect &inside, int label_width)
        : inside_ (inside), row_ (inside.y), label_width_ (label_width),
          bottom_ (inside.bottom ())
    {
    }

    bool
    full () const
    {
        return row_ >= bottom_;
    }

    int
    row () const
    {
        return row_;
    }

    void
    reserve_last ()
    {
        bottom_ = std::max (inside_.y, inside_.bottom () - 1);
    }

    void
    skip ()
    {
        ++row_;
    }

    void
    text (std::string_view content, attr_t attributes)
    {
        if (full ())
            return;
        tui::put (row_, inside_.x + 1, tui::fit (content, inside_.w - 2), attributes);
        ++row_;
    }

    void
    pair (std::string_view label, const Segments &segments)
    {
        if (full ())
            return;
        tui::put (row_, inside_.x + 1, tui::fit (label, label_width_ - 1),
                  tui::attr (tui::DIM));
        put_segments (row_, inside_.x + 1 + label_width_, inside_.right () - 1, segments);
        ++row_;
    }

    void
    pair (std::string_view label, std::string_view value, attr_t attributes = 0)
    {
        pair (label, Segments{{std::string (value), attributes ? attributes : tui::attr (tui::NORMAL)}});
    }

private:
    tui::Rect inside_;
    int row_;
    int label_width_;
    int bottom_;
};

attr_t
change_attr (double percent)
{
    return tui::attr (percent < 0 ? tui::BAD : tui::GOOD);
}

std::string
upper (std::string_view text)
{
    std::string result (text);
    for (char &letter : result)
        letter = static_cast<char> (std::toupper (static_cast<unsigned char> (letter)));
    return result;
}

std::string
money (std::string_view currency, double amount, int decimals = 2)
{
    if (currency == "usd")
        return (amount < 0 ? "-$" : "$") + markets::format_price (std::abs (amount), decimals);
    if (currency == "rub")
        return markets::format_price (amount, decimals) + " ₽";
    if (currency == "eur")
        return markets::format_price (amount, decimals) + " €";
    if (currency == "btc")
        return markets::format_price (amount, 8) + " BTC";
    return markets::format_price (amount, decimals) + " " + upper (currency);
}

struct Title
{
    std::string text;
    attr_t attributes = 0;
};

Title
panel_title (std::string_view nerd, std::string_view fallback, std::string_view name,
             const Source &source, std::optional<util::TimePoint> fetched, util::TimePoint now)
{
    Title title;
    title.text = std::string (tui::icon (std::string (nerd).c_str (),
                                         std::string (fallback).c_str ()));
    if (!title.text.empty ())
        title.text += " ";
    title.text += name;
    if (fetched)
        title.text += std::format (" · {} ago", util::format_age (*fetched, now));
    Source::Status status = source.status ();
    if (!status.error.empty () && status.error_at >= status.updated)
        {
            title.text += std::format (" · {} failed {} ago", tui::icon ("", "✗"),
                                       util::format_age (status.error_at, now));
            title.attributes = tui::attr (tui::WARN);
        }
    else if (status.busy)
        title.text += " ⟳";
    return title;
}

void
wrap_error (const tui::Rect &inside, int row, std::string_view error)
{
    std::string rest (error);
    while (!rest.empty () && row < inside.bottom ())
        {
            std::string shown = tui::fit (rest, inside.w - 2);
            tui::put (row++, inside.x + 1, shown, tui::attr (tui::BAD));
            if (shown == rest)
                break;
            std::size_t cut = shown.size () - std::string ("…").size ();
            rest = "  " + rest.substr (cut);
        }
}

bool
show_failure (const tui::Rect &inside, const Source &source, bool has_data)
{
    Source::Status status = source.status ();
    bool failing = !status.error.empty () && status.error_at >= status.updated;
    if (has_data)
        {
            if (failing && inside.h >= 2)
                tui::put (inside.bottom () - 1, inside.x + 1,
                          tui::fit (std::format ("{} {}", tui::icon ("", "✗"), status.error),
                                    inside.w - 2), tui::attr (tui::WARN));
            return failing && inside.h >= 2;
        }
    if (failing)
        wrap_error (inside, inside.y, status.error);
    else
        tui::put (inside.y, inside.x + 1, "Loading…", tui::attr (tui::DIM));
    return false;
}

class XmrScreen : public Screen
{
public:
    XmrScreen (MarketSource &market, ChartSource &chart, ChainSource &chain,
               PoolsSource &pools, NodeSource *node, Settings settings)
        : market_ (market), chart_ (chart), chain_ (chain), pools_ (pools), node_ (node),
          settings_ (std::move (settings)), zone_ (util::zone (settings_.timezone))
    {
    }

    std::string title () const override
    {
        return "XMR";
    }

    std::vector<Source *> sources () const override
    {
        std::vector<Source *> all = {&market_, &chart_, &chain_, &pools_};
        if (node_)
            all.push_back (node_);
        return all;
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        if (area.empty ())
            return;
        int top_height = std::max (8, area.h * 36 / 100);
        int middle_height = std::max (8, area.h * 34 / 100);
        tui::Rect top{area.y, area.x, std::min (top_height, area.h), area.w};
        tui::Rect middle{top.bottom (), area.x,
                         std::clamp (middle_height, 0, area.bottom () - top.bottom ()), area.w};
        tui::Rect bottom{middle.bottom (), area.x, area.bottom () - middle.bottom (), area.w};
        int left_width = std::clamp (area.w * 30 / 100, 40, 60);

        tui::Rect market_cell{top.y, top.x, top.h, left_width};
        tui::Rect chart_cell{top.y, top.x + left_width, top.h, top.w - left_width};
        draw_market (market_cell, now);
        draw_chart (chart_cell, now);

        int rest = middle.w - left_width;
        int blocks_width = rest * 45 / 100;
        tui::Rect network_cell{middle.y, middle.x, middle.h, left_width};
        tui::Rect blocks_cell{middle.y, middle.x + left_width, middle.h, blocks_width};
        tui::Rect pools_cell{middle.y, blocks_cell.right (), middle.h, middle.right () - blocks_cell.right ()};
        draw_network (network_cell, now);
        draw_blocks (blocks_cell, now);
        draw_pools (pools_cell, now);

        if (bottom.h >= 3)
            {
                tui::Rect calculator_cell{bottom.y, bottom.x, bottom.h, left_width};
                tui::Rect node_cell{bottom.y, bottom.x + left_width, bottom.h, bottom.w - left_width};
                draw_calculator (calculator_cell, now);
                draw_node (node_cell, now);
            }
    }

private:
    std::optional<double>
    current_reward () const
    {
        if (auto pools = pools_.pools.get ())
            for (const auto &entry : *pools)
                if (entry && entry->value.main_reward)
                    return static_cast<double> (entry->value.main_reward) / atomic_units;
        if (auto blocks = chain_.blocks.get (); blocks && !blocks->value.empty ())
            return static_cast<double> (tail_emission + blocks->value.front ().fees)
                   / atomic_units;
        return std::nullopt;
    }

    void
    draw_market (const tui::Rect &cell, util::TimePoint now)
    {
        auto current = market_.market.get ();
        Title title = panel_title ("\U000F01C1", "", "Market · CoinGecko", market_,
                                   current ? std::optional (current->fetched) : std::nullopt, now);
        tui::Rect inside = tui::frame (cell, title.text, title.attributes);
        if (inside.empty ())
            return;
        bool failing = show_failure (inside, market_, current != nullptr);
        if (!current)
            return;
        const Market &market = current->value;
        Lines lines (inside, 12);
        if (failing)
            lines.reserve_last ();

        double usd = market.price.at ("usd");
        int row = lines.row ();
        int column = inside.x + 1;
        column += tui::put (row, column, std::format ("XMR  ${}", markets::format_price (usd, 2)),
                            tui::attr (tui::NORMAL) | A_BOLD);
        std::string arrow = market.change_24h_percent < 0 ? tui::icon ("\U000F0533", "▼")
                            : tui::icon ("\U000F0535", "▲");
        tui::put (row, column + 2, std::format ("{} {} 24h", arrow,
                                                format_percent (market.change_24h_percent)),
                  change_attr (market.change_24h_percent), inside.right () - column - 3);
        lines.skip ();

        Segments others;
        for (std::string_view currency :
                {
                    "rub", "eur", "btc"
                })
            if (auto found = market.price.find (std::string (currency)); found != market.price.end ())
                {
                    if (!others.empty ())
                        others.emplace_back (" · ", tui::attr (tui::FRAME));
                    others.emplace_back (currency == "btc"
                                         ? std::format ("{:.6f} BTC", found->second)
                                         : money (currency, found->second, 0),
                                         tui::attr (tui::NORMAL));
                }
        lines.pair ("also", others);
        lines.pair ("24h range", std::format ("${} – ${}",
                                              markets::format_price (market.low_24h_usd, 2),
                                              markets::format_price (market.high_24h_usd, 2)));
        Segments short_changes =
        {
            {"7d ", tui::attr (tui::DIM)},
            {format_percent (market.change_7d_percent), change_attr (market.change_7d_percent)},
            {"  30d ", tui::attr (tui::DIM)},
            {format_percent (market.change_30d_percent), change_attr (market.change_30d_percent)},
        };
        lines.pair ("change", short_changes);
        lines.pair ("", Segments{{"60d ", tui::attr (tui::DIM)},
            {format_percent (market.change_60d_percent), change_attr (market.change_60d_percent)},
            {"  1y ", tui::attr (tui::DIM)},
            {format_percent (market.change_1y_percent), change_attr (market.change_1y_percent)}});
        lines.pair ("market cap", std::format ("${}{}", format_compact (market.market_cap_usd),
                                               market.rank ? std::format ("  rank #{}", market.rank)
                                               : std::string{}));
        lines.pair ("volume 24h", "$" + format_compact (market.volume_usd));
        lines.pair ("supply", markets::format_price (market.circulating_supply, 0) + " XMR");
        lines.pair ("emission", Segments{{"0.6 XMR/block", tui::attr (tui::NORMAL)},
            {" · ", tui::attr (tui::FRAME)},
            {std::format ("{:.2f}%/year", yearly_inflation_percent (market.circulating_supply)), tui::attr (tui::DIM)}});
        if (auto ath = market.ath.find ("usd"); ath != market.ath.end ())
            {
                std::string date = market.ath_date
                                   ? std::format ("{:%d %b %Y}", floor<days> (*market.ath_date))
                                   : std::string{};
                lines.pair ("ATH", std::format ("${}  {}", markets::format_price (ath->second, 2),
                                                date));
                double from_ath = market.ath_change_percent.contains ("usd")
                                  ? market.ath_change_percent.at ("usd")
                                  : (usd - ath->second) / ath->second * 100;
                lines.pair ("from ATH", Segments{{format_percent (from_ath), change_attr (from_ath)}});
            }
        if (market.updated)
            lines.pair ("priced", util::format_age (*market.updated, now) + " ago",
                        tui::attr (tui::DIM));
    }

    void
    draw_chart (const tui::Rect &cell, util::TimePoint now)
    {
        auto current = chart_.chart.get ();
        std::string name = "XMR/USD · 30 days";
        if (current)
            name += " · " + current->value.provider;
        Title title = panel_title ("\U000F012A", "", name, chart_,
                                   current ? std::optional (current->fetched) : std::nullopt, now);
        tui::Rect inside = tui::frame (cell, title.text, title.attributes);
        if (inside.empty ())
            return;
        bool failing = show_failure (inside, chart_, current != nullptr);
        if (!current)
            return;
        const std::vector<markets::Point> &points = current->value.points;
        markets::Statistics stats = markets::statistics (points);
        int decimals = 2;
        double change = stats.last - stats.open;
        int column = inside.x + 1;
        column += tui::put (inside.y, column, "$" + markets::format_price (stats.last, decimals),
                            tui::attr (tui::NORMAL) | A_BOLD) + 2;
        column += tui::put (inside.y, column, std::format ("{} ({}) in 30 days",
                            (change < 0 ? "−$" : "+$") + markets::format_price (std::abs (change), decimals),
                            format_percent (stats.change_percent)),
                            change_attr (change), inside.right () - column - 1);
        std::string summary = std::format ("high ${}  low ${}  avg ${}  vol {:.2f}%/day",
                                           markets::format_price (stats.high, decimals),
                                           markets::format_price (stats.low, decimals),
                                           markets::format_price (stats.average, decimals),
                                           stats.daily_volatility_percent);
        int summary_width = tui::width (summary);
        if (inside.right () - summary_width - 1 > column + 2)
            tui::put (inside.y, inside.right () - summary_width - 1, summary, tui::attr (tui::DIM));

        int plot_height = inside.h - 2 - (failing ? 1 : 0);
        if (plot_height < 2)
            return;
        markets::Axis axis = markets::nice_axis (stats.low, stats.high,
                             std::clamp (plot_height / 2 + 1, 2, 7));
        int label_decimals = std::max (axis.decimals, 0);
        int label_width = 0;
        for (double tick : axis.ticks)
            label_width = std::max (label_width, tui::width (markets::format_price (tick,
                                    label_decimals)));
        int plot_x = inside.x + label_width + 2;
        int plot_width = inside.right () - plot_x;
        if (plot_width < 4)
            return;
        int plot_top = inside.y + 1;

        tui::Braille canvas (plot_width, plot_height);
        auto dot_row = [&] (double value)
        {
            double fraction = (axis.hi - value) / (axis.hi - axis.lo);
            return static_cast<int> (std::lround (fraction * (canvas.dots_h () - 1)));
        };
        util::TimePoint end = points.back ().t;
        util::TimePoint start = end - chart_span;
        auto dot_column = [&] (util::TimePoint when)
        {
            double fraction = duration<double> (when - start) / duration<double> (end - start);
            return static_cast<int> (std::lround (std::clamp (fraction, 0.0, 1.0)
                                                  * (canvas.dots_w () - 1)));
        };
        for (std::size_t index = 0; index < points.size (); ++index)
            {
                int dot_x = dot_column (points[index].t);
                int dot_y = dot_row (points[index].v);
                if (index > 0 && points[index].t - points[index - 1].t <= hours{24})
                    canvas.line (dot_column (points[index - 1].t), dot_row (points[index - 1].v),
                                 dot_x, dot_y);
                else
                    canvas.set (dot_x, dot_y);
            }

        attr_t frame_attr = tui::attr (tui::FRAME);
        for (int line = 0; line < plot_height; ++line)
            tui::put (plot_top + line, plot_x - 1, "│", frame_attr);
        for (double tick : axis.ticks)
            {
                int line = dot_row (tick) / 4;
                if (line < 0 || line >= plot_height)
                    continue;
                tui::put (plot_top + line, inside.x,
                          tui::pad (markets::format_price (tick, label_decimals), label_width, 'r'),
                          tui::attr (tui::DIM));
                tui::put (plot_top + line, plot_x - 1, "┤", frame_attr);
                for (int dot = 0; dot < plot_width; dot += 2)
                    if (!canvas.used (dot, line))
                        tui::put (plot_top + line, plot_x + dot, "·", frame_attr);
            }
        canvas.draw (plot_top, plot_x, tui::attr (tui::PLOT_ORANGE));

        int axis_row = plot_top + plot_height;
        tui::put (axis_row, plot_x - 1, "└", frame_attr);
        auto midnight = floor<days> (util::to_local (start, zone_)) + days{1};
        int next_free = plot_x;
        for (; zone_->to_sys (midnight) <= end; midnight += days{1})
            {
                if (weekday{midnight} != Monday)
                    continue;
                int label_column = plot_x + dot_column (zone_->to_sys (midnight)) / 2;
                std::string label = "╵" + std::format ("{:%d %b}", midnight);
                int label_columns = tui::width (label);
                if (label_column < next_free || label_column + label_columns > inside.right ())
                    continue;
                tui::put (axis_row, label_column, label, tui::attr (tui::DIM));
                next_free = label_column + label_columns + 2;
            }
    }

    void
    draw_network (const tui::Rect &cell, util::TimePoint now)
    {
        auto info = chain_.info.get ();
        auto blocks = chain_.blocks.get ();
        Title title = panel_title ("\U000F01A7", "", "Network · xmrchain", chain_,
                                   info ? std::optional (info->fetched) : std::nullopt, now);
        tui::Rect inside = tui::frame (cell, title.text, title.attributes);
        if (inside.empty ())
            return;
        bool failing = show_failure (inside, chain_, info != nullptr);
        if (!info)
            return;
        const NetworkInfo &network = info->value;
        Lines lines (inside, 13);
        if (failing)
            lines.reserve_last ();
        lines.pair ("height", markets::format_price (static_cast<double> (network.height), 0),
                    tui::attr (tui::NORMAL) | A_BOLD);
        lines.pair ("difficulty", format_compact (static_cast<double> (network.difficulty)));
        lines.pair ("hashrate", format_hashrate (network.hashrate), tui::attr (tui::ACCENT));
        if (blocks && !blocks->value.empty ())
            {
                const std::vector<Block> &recent = blocks->value;
                if (auto average = average_block_time (recent))
                    {
                        tui::Role role = *average > seconds{180} ? tui::WARN : tui::NORMAL;
                        lines.pair ("block time", std::format ("{} avg of last {}",
                                                               format_duration (*average),
                                                               recent.size ()),
                                    tui::attr (role));
                    }
                seconds age = duration_cast<seconds> (now - recent.front ().timestamp);
                lines.pair ("last block", util::format_age (recent.front ().timestamp, now) + " ago",
                            tui::attr (age > minutes{10} ? tui::WARN : tui::NORMAL));
                std::uint64_t fees = 0;
                int transactions = 0;
                for (const Block &block : recent)
                    {
                        fees += block.fees;
                        transactions += block.transactions;
                    }
                lines.pair ("fees/block", format_xmr (static_cast<double> (fees) / atomic_units
                                                      / static_cast<double> (recent.size ()), 5) + " XMR");
                if (transactions)
                    lines.pair ("fee/tx", format_xmr (static_cast<double> (fees) / atomic_units
                                                      / transactions, 6) + " XMR");
            }
        if (auto reward = current_reward ())
            lines.pair ("reward", format_xmr (*reward, 4) + " XMR");
        lines.pair ("tx pool", std::format ("{} txs · {}", network.tx_pool_size,
                                            util::human_bytes (network.tx_pool_bytes)));
        lines.pair ("transactions", markets::format_price (static_cast<double> (network.tx_count),
                    0));
        lines.pair ("hard fork", std::format ("v{} · median block {}", network.hard_fork,
                                              util::human_bytes (network.block_size_median)));
    }

    void
    draw_blocks (const tui::Rect &cell, util::TimePoint now)
    {
        auto blocks = chain_.blocks.get ();
        Title title = panel_title ("\U000F01A7", "", "Recent blocks", chain_,
                                   blocks ? std::optional (blocks->fetched) : std::nullopt, now);
        tui::Rect inside = tui::frame (cell, title.text, title.attributes);
        if (inside.empty ())
            return;
        if (!blocks)
            {
                show_failure (inside, chain_, false);
                return;
            }
        constexpr int height_width = 10;
        constexpr int age_width = 6;
        constexpr int count_width = 5;
        constexpr int size_width = 8;
        constexpr int fees_width = 10;
        bool show_size = inside.w >= height_width + age_width + count_width + size_width + 2;
        bool show_fees = inside.w >= height_width + age_width + count_width + size_width
                         + fees_width + 2;
        std::vector<std::pair<std::string, int>> columns =
        {
            {"height", height_width}, {"age", age_width}, {"txs", count_width}
        };
        if (show_size)
            columns.emplace_back ("size", size_width);
        if (show_fees)
            columns.emplace_back ("fees XMR", fees_width);
        int row = inside.y;
        int column = inside.x + 1;
        for (const auto &[header, width] : columns)
            {
                tui::put (row, column, tui::pad (header, width - 1, header == "height" ? 'l' : 'r'),
                          tui::attr (tui::DIM) | A_BOLD);
                column += width;
            }
        ++row;
        for (const Block &block : blocks->value)
            {
                if (row >= inside.bottom ())
                    break;
                column = inside.x + 1;
                std::vector<std::string> cells =
                {
                    markets::format_price (static_cast<double> (block.height), 0),
                    util::format_age (block.timestamp, now),
                    std::to_string (block.transactions),
                };
                if (show_size)
                    cells.push_back (util::human_bytes (block.size));
                if (show_fees)
                    cells.push_back (format_xmr (static_cast<double> (block.fees) / atomic_units, 5));
                for (std::size_t index = 0; index < cells.size (); ++index)
                    {
                        attr_t attributes = tui::attr (index == 0 ? tui::TITLE : tui::NORMAL);
                        if (index == 2 && block.transactions == 0)
                            attributes = tui::attr (tui::DIM);
                        tui::put (row, column, tui::pad (cells[index], columns[index].second - 1,
                                                         index == 0 ? 'l' : 'r'), attributes);
                        column += columns[index].second;
                    }
                ++row;
            }
    }

    void
    draw_pools (const tui::Rect &cell, util::TimePoint now)
    {
        auto pools = pools_.pools.get ();
        std::optional<util::TimePoint> oldest;
        bool any = false;
        if (pools)
            for (const auto &entry : *pools)
                if (entry)
                    {
                        any = true;
                        oldest = oldest ? std::min (*oldest, entry->fetched) : entry->fetched;
                    }
        Title title = panel_title ("\U000F0849", "", "P2Pool · p2pool.observer", pools_, oldest,
                                   now);
        tui::Rect inside = tui::frame (cell, title.text, title.attributes);
        if (inside.empty ())
            return;
        bool failing = show_failure (inside, pools_, any);
        if (!any)
            return;
        const std::vector<std::string> &names = pools_.names ();
        constexpr int label_width = 11;
        int column_width = std::max (8, (inside.w - 2 - label_width)
                                     / std::max<int> (1, static_cast<int> (names.size ())));
        bool tight = column_width < 11;
        int bottom_row = inside.bottom () - (failing ? 1 : 0);
        int row = inside.y;
        auto put_row = [&] (std::string_view label, auto value_of, auto role_of)
        {
            if (row >= bottom_row)
                return;
            tui::put (row, inside.x + 1, label, tui::attr (tui::DIM));
            for (std::size_t index = 0; index < names.size (); ++index)
                {
                    int column = inside.x + 1 + label_width
                                 + static_cast<int> (index) * column_width;
                    if (column + column_width > inside.right ())
                        break;
                    const auto &entry = (*pools)[index];
                    if (!entry)
                        {
                            tui::put (row, column, tui::pad ("—", column_width - 1, 'r'),
                                      tui::attr (tui::DIM));
                            continue;
                        }
                    tui::put (row, column, tui::pad (tui::fit (value_of (entry->value),
                                                     column_width - 1), column_width - 1, 'r'),
                              tui::attr (role_of (entry->value)));
                }
            ++row;
        };
        auto normal = [] (const Pool &)
        {
            return tui::NORMAL;
        };
        put_row ("", [] (const Pool &pool)
        {
            return pool.name;
        }, [] (const Pool &)
        {
            return tui::TITLE;
        });
        put_row ("hashrate", [tight] (const Pool &pool)
        {
            std::string text = format_hashrate (pool.hashrate);
            if (tight)
                std::erase (text, ' ');
            return text;
        }, [] (const Pool &)
        {
            return tui::ACCENT;
        });
        double network_hashrate = 0;
        if (auto info = chain_.info.get ())
            network_hashrate = info->value.hashrate;
        if (network_hashrate > 0)
            put_row ("of network", [network_hashrate] (const Pool &pool)
            {
                return std::format ("{:.2f}%", pool.hashrate / network_hashrate * 100);
            }, normal);
        put_row ("miners", [] (const Pool &pool)
        {
            return markets::format_price (pool.active_miners, 0);
        }, normal);
        put_row ("all-time", [] (const Pool &pool)
        {
            return markets::format_price (pool.total_miners, 0);
        }, [] (const Pool &)
        {
            return tui::DIM;
        });
        put_row ("share diff", [] (const Pool &pool)
        {
            return format_compact (static_cast<double> (pool.difficulty));
        }, normal);
        put_row ("last block", [now] (const Pool &pool)
        {
            return util::format_age (pool.last_found_at, now) + " ago";
        }, normal);
        put_row ("at height", [] (const Pool &pool)
        {
            return std::to_string (pool.last_found_height);
        }, [] (const Pool &)
        {
            return tui::DIM;
        });
        put_row ("effort", [] (const Pool &pool)
        {
            return std::format ("{:.0f}%", pool.effort_current);
        }, [] (const Pool &pool)
        {
            return pool.effort_current > 300 ? tui::WARN : tui::NORMAL;
        });
        put_row ("avg effort", [] (const Pool &pool)
        {
            return std::format ("{:.0f}%", pool.effort_average);
        }, normal);
        put_row ("found", [] (const Pool &pool)
        {
            return markets::format_price (pool.blocks_found, 0);
        }, normal);
        put_row ("version", [] (const Pool &pool)
        {
            return pool.p2pool_version;
        }, [] (const Pool &)
        {
            return tui::DIM;
        });
    }

    void
    draw_calculator (const tui::Rect &cell, util::TimePoint now)
    {
        (void) now;
        std::string name = std::format ("Mining · {}", format_hashrate (settings_.hashrate));
        tui::Rect inside = tui::frame (cell, std::string (tui::icon ("\U000F08B7", "")) +
                                       (tui::nerd_icons () ? " " : "") + name);
        if (inside.empty ())
            return;
        auto info = chain_.info.get ();
        auto reward = current_reward ();
        Lines lines (inside, 12);
        if (!info || !reward)
            {
                lines.text ("Waiting for network data…", tui::attr (tui::DIM));
                return;
            }
        auto market = market_.market.get ();
        double usd = market ? market->value.price.at ("usd") : 0;
        std::optional<double> local;
        if (market)
            if (auto found = market->value.price.find (settings_.currency);
                    found != market->value.price.end ())
                local = found->second;
        Estimate result = estimate (settings_.hashrate, info->value.hashrate, *reward,
                                    settings_.watts, settings_.price_per_kwh);
        lines.pair ("setup", std::format ("{} W · {} /kWh", settings_.watts,
                                          money (settings_.currency, settings_.price_per_kwh)));
        lines.pair ("net share", std::format ("{:.6f}%", result.share * 100));
        auto amounts = [&] (double xmr)
        {
            Segments segments = {{format_xmr (xmr, 6) + " XMR", tui::attr (tui::ACCENT)}};
            if (usd > 0)
                {
                    segments.emplace_back (" · ", tui::attr (tui::FRAME));
                    segments.emplace_back (money ("usd", xmr * usd), tui::attr (tui::NORMAL));
                }
            if (local && settings_.currency != "usd")
                {
                    segments.emplace_back (" · ", tui::attr (tui::FRAME));
                    segments.emplace_back (money (settings_.currency, xmr * *local),
                                           tui::attr (tui::NORMAL));
                }
            return segments;
        };
        lines.pair ("per day", amounts (result.xmr_per_day));
        lines.pair ("per month", amounts (result.xmr_per_day * 30));
        lines.pair ("power", std::format ("{:.2f} kWh/day · {}", result.kwh_per_day,
                                          money (settings_.currency, result.power_cost_per_day)));
        if (local)
            {
                double profit = result.xmr_per_day * *local - result.power_cost_per_day;
                lines.pair ("profit/day", Segments{{
                        money (settings_.currency, profit),
                        tui::attr (profit < 0 ? tui::BAD : tui::GOOD)
                    }});
                if (result.xmr_per_day > 0)
                    lines.pair ("break-even", money (settings_.currency,
                                                     result.power_cost_per_day / result.xmr_per_day, 0)
                                + " /XMR", tui::attr (tui::DIM));
            }
        lines.pair ("solo block", std::format ("every {} on average",
                                               format_duration (result.solo_block_time)));
        if (auto pools = pools_.pools.get ())
            for (const auto &entry : *pools)
                if (entry)
                    {
                        double shares = pool_shares_per_day (settings_.hashrate, entry->value);
                        if (shares <= 0)
                            continue;
                        attr_t attributes = tui::attr (shares < 1 ? tui::WARN : tui::NORMAL);
                        lines.pair ("p2pool " + entry->value.name,
                        Segments{{std::format ("{:.1f} shares/day", shares), attributes},
                            {" · ", tui::attr (tui::FRAME)},
                            {
                                "one every " + format_duration (seconds{static_cast<long long> (86400 / shares)}),
                                tui::attr (tui::DIM)
                            }});
                    }
        lines.pair ("reward", format_xmr (*reward, 4) + " XMR/block", tui::attr (tui::DIM));
    }

    void
    draw_node (const tui::Rect &cell, util::TimePoint now)
    {
        if (!node_)
            {
                tui::Rect inside = tui::frame (cell, std::string (tui::icon ("\U000F048B", "")) +
                                               (tui::nerd_icons () ? " " : "") + "Own node");
                if (inside.empty ())
                    return;
                Lines lines (inside, 0);
                lines.text ("No Monero node configured.", tui::attr (tui::DIM));
                lines.skip ();
                lines.text ("Run monerod with RPC on the LAN, for example:", tui::attr (tui::NORMAL));
                lines.text ("  monerod --rpc-bind-ip 0.0.0.0 --confirm-external-bind --restricted-rpc",
                            tui::attr (tui::ACCENT));
                lines.text ("then add to config.toml:", tui::attr (tui::NORMAL));
                lines.text ("  [xmr]", tui::attr (tui::ACCENT));
                lines.text ("  node = \"http://192.168.1.10:18081\"", tui::attr (tui::ACCENT));
                lines.skip ();
                lines.text ("See 'info raspdash XMR' for details.", tui::attr (tui::DIM));
                return;
            }
        auto state = node_->node.get ();
        Title title = panel_title ("\U000F048B", "", "Node · " + node_->address (), *node_,
                                   state ? std::optional (state->fetched) : std::nullopt, now);
        tui::Rect inside = tui::frame (cell, title.text, title.attributes);
        if (inside.empty ())
            return;
        bool failing = show_failure (inside, *node_, state != nullptr);
        if (!state)
            return;
        const NodeInfo &node = state->info;
        double fraction = sync_fraction (node);
        std::uint64_t target = std::max (node.target_height, node.height);
        std::uint64_t behind = target - node.height;

        int row = inside.y;
        Segments status;
        if (node.offline)
            status.emplace_back ("● offline", tui::attr (tui::BAD));
        else if (node.synchronized && behind == 0)
            status.emplace_back (std::string (tui::icon ("", "●")) + " synchronized",
                                 tui::attr (tui::GOOD));
        else
            status.emplace_back (std::format ("⟳ syncing {:.2f}%", fraction * 100),
                                 tui::attr (tui::WARN) | A_BOLD);
        status.emplace_back (std::format ("   height {}", markets::format_price (
                                              static_cast<double> (node.height), 0)),
                             tui::attr (tui::NORMAL) | A_BOLD);
        if (behind)
            status.emplace_back (std::format (" / {} · {} blocks behind",
                                              markets::format_price (static_cast<double> (target), 0),
                                              markets::format_price (static_cast<double> (behind), 0)),
                                 tui::attr (tui::NORMAL));
        if (behind && state->blocks_per_second && *state->blocks_per_second > 0)
            status.emplace_back (std::format (" · {:.1f} blocks/s · ETA {}",
                                              *state->blocks_per_second,
                                              format_duration (seconds{static_cast<long long> (
                                                      static_cast<double> (behind) / *state->blocks_per_second)})),
                                 tui::attr (tui::DIM));
        if (auto chain = chain_.info.get (); chain && !behind)
            {
                long long lag = static_cast<long long> (chain->value.height)
                                - static_cast<long long> (node.height);
                if (lag > 2)
                    status.emplace_back (std::format (" · {} behind explorer", lag), tui::attr (tui::WARN));
            }
        put_segments (row++, inside.x + 1, inside.right () - 1, status);
        if (row < inside.bottom ())
            tui::gauge (row++, inside.x + 1, inside.w - 2, fraction,
                        tui::attr (fraction >= 1 ? tui::GOOD : tui::WARN));
        if (row < inside.bottom ())
            ++row;

        tui::Rect body{row, inside.x, inside.bottom () - row - (failing ? 1 : 0), inside.w};
        if (body.h <= 0)
            return;
        int halves = body.w >= 70 ? 2 : 1;
        std::vector<tui::Rect> parts = body.columns (halves, 1);
        Lines left (parts[0], 14);
        Lines right (parts[halves - 1], 14);
        Lines &second = halves == 2 ? right : left;

        auto unknown = [&node] (std::string text)
        {
            return text.empty () || node.restricted ? std::string ("hidden (restricted)") : text;
        };
        left.pair ("network", node.nettype.empty () ? std::string ("?") : node.nettype);
        left.pair ("difficulty", format_compact (static_cast<double> (node.difficulty)));
        left.pair ("node hashrate", format_hashrate (static_cast<double> (node.difficulty)
                   / target_block_seconds));
        left.pair ("tx pool", std::format ("{} txs", node.tx_pool_size));
        left.pair ("transactions", markets::format_price (static_cast<double> (node.tx_count), 0));
        left.pair ("database", util::human_bytes (node.database_size));
        left.pair ("free space", node.free_space ? util::human_bytes (*node.free_space)
                   : unknown (""), node.free_space && *node.free_space < 20ULL << 30
                   ? tui::attr (tui::BAD) : 0);

        second.pair ("connections", node.restricted && !node.incoming_connections
                     && !node.outgoing_connections ? unknown ("")
                     : std::format ("{} in · {} out", node.incoming_connections,
                                    node.outgoing_connections),
                     tui::attr (node.outgoing_connections || node.restricted ? tui::NORMAL : tui::BAD));
        second.pair ("RPC clients", std::to_string (node.rpc_connections));
        second.pair ("peer lists", node.restricted && !node.white_peers ? unknown ("")
                     : std::format ("{} white · {} grey", node.white_peers, node.grey_peers));
        second.pair ("version", node.version.empty () ? unknown ("") : node.version);
        second.pair ("uptime", node.start_time
                     ? format_duration (duration_cast<seconds> (now - *node.start_time))
                     : unknown (""));
        second.pair ("RPC mode", node.restricted ? "restricted" : "full",
                     tui::attr (tui::DIM));
        if (node.update_available)
            second.pair ("update", "a new monerod release is available", tui::attr (tui::WARN));
        if (node.busy_syncing)
            second.pair ("busy", "syncing blocks", tui::attr (tui::WARN));
        if (!node.top_block_hash.empty ())
            left.pair ("top block", node.top_block_hash.substr (0, 16) + "…", tui::attr (tui::DIM));
    }

    MarketSource &market_;
    ChartSource &chart_;
    ChainSource &chain_;
    PoolsSource &pools_;
    NodeSource *node_;
    Settings settings_;
    const std::chrono::time_zone *zone_;
};
}
}

void
install_xmr (App &app)
{
    using namespace xmr;
    const toml::table &table = app.config ().section ("xmr");
    auto interval_of = [&table] (std::string_view key, int fallback)
    {
        return seconds{std::max (10, table[key].value_or (fallback))};
    };
    Settings settings;
    settings.hashrate = table["hashrate_hs"].value_or (10000.0);
    settings.watts = table["watts"].value_or (65.0);
    settings.price_per_kwh = table["price_per_kwh"].value_or (6.5);
    settings.currency = table["currency"].value_or<std::string> ("rub");
    std::ranges::transform (settings.currency, settings.currency.begin (), [] (unsigned char letter)
    {
        return static_cast<char> (std::tolower (letter));
    });
    settings.timezone = table["timezone"].value_or<std::string> ("Europe/Moscow");
    if (settings.hashrate < 0 || settings.watts < 0 || settings.price_per_kwh < 0)
        throw std::runtime_error ("[xmr] hashrate_hs, watts and price_per_kwh must not be negative");

    std::string explorer = table["explorer"].value_or<std::string> (std::string (explorer_default));
    while (explorer.ends_with ('/'))
        explorer.pop_back ();

    std::vector<std::string> pool_names;
    if (const toml::array *names = table["pools"].as_array ())
        for (const toml::node &entry : *names)
            {
                std::string name = entry.value_or<std::string> ("");
                if (name != "main" && name != "mini" && name != "nano")
                    throw std::runtime_error (std::format (
                                                  "[xmr] pools: unknown pool '{}' (use main, mini or nano)", name));
                pool_names.push_back (name);
            }
    else
        pool_names = {"main", "mini", "nano"};

    auto &market = app.add_source (std::make_unique<MarketSource> (
                                       interval_of ("market_interval", 600)));
    auto &chart = app.add_source (std::make_unique<ChartSource> (
                                      interval_of ("chart_interval", 1800)));
    auto &chain = app.add_source (std::make_unique<ChainSource> (
                                      explorer, interval_of ("network_interval", 120)));
    auto &pools = app.add_source (std::make_unique<PoolsSource> (
                                      std::move (pool_names), interval_of ("pools_interval", 300)));
    NodeSource *node = nullptr;
    std::string node_address = table["node"].value_or<std::string> ("");
    if (!util::trim (node_address).empty ())
        node = &app.add_source (std::make_unique<NodeSource> (
                                    node_rpc_url (node_address), table["node_direct"].value_or (true),
                                    interval_of ("node_interval", 30)));
    app.add_screen (std::make_unique<XmrScreen> (market, chart, chain, pools, node,
                    std::move (settings)));
}
