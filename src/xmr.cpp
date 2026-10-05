/* xmr.cpp -- the XMR screen: Monero market, network and node.

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

namespace
{
template <typename T>
struct Stamped
{
    T value;
    util::TimePoint fetched{};
};

constexpr std::string_view coingecko_coin
    = "https://api.coingecko.com/api/v3/coins/monero?localization=false"
      "&tickers=false&market_data=true&community_data=false&developer_data=false"
      "&sparkline=false";
constexpr std::string_view coingecko_chart
    = "https://api.coingecko.com/api/v3/coins/monero/market_chart"
      "?vs_currency=usd&days=30";
constexpr std::string_view explorer_default = "https://xmrchain.net";
constexpr days chart_span{30};

std::vector<markets::Point>
chart_from (std::string_view body)
{
    std::vector<markets::Point> points = markets::last_span (markets::parse_coingecko (body),
                                         chart_span);
    if (points.size () < 2)
        throw std::runtime_error ("CoinGecko: too few prices");
    return points;
}

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

    Latest<Stamped<std::vector<markets::Point>>> chart;

protected:
    void
    fetch () override
    {
        std::string body = http::get (std::string (coingecko_chart));
        std::vector<markets::Point> parsed = chart_from (body);
        cache_put ("chart.json", body);
        chart.set ({std::move (parsed), util::Clock::now ()});
    }

    std::optional<util::TimePoint>
    restore () override
    {
        auto cached = cache_get ("chart.json");
        if (!cached)
            return std::nullopt;
        chart.set ({chart_from (cached->body), cached->saved});
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

protected:
    void
    fetch () override
    {
        std::string info_body = http::get (explorer_ + "/api/networkinfo");
        NetworkInfo parsed_info = parse_network_info (info_body);
        cache_put ("networkinfo.json", info_body);
        info.set ({std::move (parsed_info), util::Clock::now ()});
    }

    std::optional<util::TimePoint>
    restore () override
    {
        auto cached = cache_get ("networkinfo.json");
        if (!cached)
            return std::nullopt;
        info.set ({parse_network_info (cached->body), cached->saved});
        return cached->saved;
    }

private:
    std::string explorer_;
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
    XmrScreen (MarketSource &market, ChartSource &chart, ChainSource &chain, NodeSource *node, Settings settings)
        : market_ (market), chart_ (chart), chain_ (chain), node_ (node),
          settings_ (std::move (settings)), zone_ (util::zone (settings_.timezone))
    {
    }

    std::string title () const override
    {
        return "XMR";
    }

    std::vector<Source *> sources () const override
    {
        std::vector<Source *> all = {&market_, &chart_, &chain_};
        if (node_)
            all.push_back (node_);
        return all;
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        if (area.empty ())
            return;
        int bottom_height = std::clamp (area.h * 30 / 100, std::min (area.h / 2, 15), 18);
        tui::Rect top{area.y, area.x, area.h - bottom_height, area.w};
        tui::Rect bottom{top.bottom (), area.x, area.bottom () - top.bottom (), area.w};
        int left_width = std::clamp (area.w * 30 / 100, 40, 60);

        tui::Rect market_cell{top.y, top.x, top.h, left_width};
        tui::Rect chart_cell{top.y, top.x + left_width, top.h, top.w - left_width};
        draw_market (market_cell, now);
        draw_chart (chart_cell, now);

        if (bottom.h < 3)
            return;
        int calculator_width = std::clamp (bottom.w * 30 / 100, 40, 60);
        if (bottom.w - left_width - calculator_width < 40)
            calculator_width = std::max (0, bottom.w - left_width) / 2;
        tui::Rect network_cell{bottom.y, bottom.x, bottom.h, left_width};
        tui::Rect calculator_cell{bottom.y, network_cell.right (), bottom.h, calculator_width};
        tui::Rect node_cell{bottom.y, calculator_cell.right (), bottom.h,
                            bottom.right () - calculator_cell.right ()};
        draw_network (network_cell, now);
        draw_calculator (calculator_cell, now);
        draw_node (node_cell, now);
    }

private:
    static double
    reward ()
    {
        return static_cast<double> (tail_emission) / atomic_units;
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
        std::string name = "XMR/USD · 30 days · CoinGecko";
        Title title = panel_title ("\U000F012A", "", name, chart_,
                                   current ? std::optional (current->fetched) : std::nullopt, now);
        tui::Rect inside = tui::frame (cell, title.text, title.attributes);
        if (inside.empty ())
            return;
        bool failing = show_failure (inside, chart_, current != nullptr);
        if (!current)
            return;
        const std::vector<markets::Point> &points = current->value;
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
        lines.pair ("block time", format_duration (seconds{static_cast<long long> (target_block_seconds)})
                    + " target");
        lines.pair ("reward", format_xmr (reward (), 1) + " XMR tail emission");
        lines.pair ("tx pool", std::format ("{} txs · {}", network.tx_pool_size,
                                            util::human_bytes (network.tx_pool_bytes)));
        lines.pair ("transactions", markets::format_price (static_cast<double> (network.tx_count),
                    0));
        lines.pair ("hard fork", std::format ("v{}", network.hard_fork));
        lines.pair ("median block", util::human_bytes (network.block_size_median));
        lines.pair ("peers", std::format ("{} in · {} out", network.incoming_connections,
                                          network.outgoing_connections), tui::attr (tui::DIM));
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
        Lines lines (inside, 12);
        if (!info)
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
        Estimate result = estimate (settings_.hashrate, info->value.hashrate, reward (),
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
        lines.pair ("reward", format_xmr (reward (), 1) + " XMR/block, no fees",
                    tui::attr (tui::DIM));
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

    auto &market = app.add_source (std::make_unique<MarketSource> (
                                       interval_of ("market_interval", 600)));
    auto &chart = app.add_source (std::make_unique<ChartSource> (
                                      interval_of ("chart_interval", 1800)));
    auto &chain = app.add_source (std::make_unique<ChainSource> (
                                      explorer, interval_of ("network_interval", 120)));
    NodeSource *node = nullptr;
    std::string node_address = table["node"].value_or<std::string> ("");
    if (!util::trim (node_address).empty ())
        node = &app.add_source (std::make_unique<NodeSource> (
                                    node_rpc_url (node_address), table["node_direct"].value_or (true),
                                    interval_of ("node_interval", 30)));
    app.add_screen (std::make_unique<XmrScreen> (market, chart, chain, node,
                    std::move (settings)));
}
