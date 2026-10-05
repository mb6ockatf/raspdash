/* markets.cpp -- price charts over a week, a month or half a year.

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

#include "markets.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <stdexcept>
#include <optional>
#include <ranges>
#include <thread>

#include <nlohmann/json.hpp>

#include "app.hpp"
#include "http.hpp"
#include "modules.hpp"

using json = nlohmann::json;
using namespace std::chrono;

namespace markets
{
namespace
{
double
to_double (std::string_view s)
{
    double v = 0;
    auto r = std::from_chars (s.data (), s.data () + s.size (), v);
    if (r.ec != std::errc ())
        throw std::runtime_error (std::format ("bad number '{}'", s));
    return v;
}

double
num (const json &j)
{
    if (j.is_number ())
        return j.get<double> ();
    if (j.is_string ())
        return to_double (j.get_ref<const std::string &> ());
    throw std::runtime_error ("expected a number");
}

util::TimePoint
from_ms (const json &j)
{
    return util::TimePoint{milliseconds{static_cast<long long> (num (j))}};
}

util::TimePoint
from_s (const json &j)
{
    return util::TimePoint{seconds{static_cast<long long> (num (j))}};
}

void
sort_points (std::vector<Point> &pts)
{
    std::ranges::sort (pts, {}, &Point::t);
    auto dup = std::ranges::unique (pts, {}, &Point::t);
    pts.erase (dup.begin (), dup.end ());
}

std::vector<Point>
candles (const json &rows, bool ms, std::size_t close_idx)
{
    std::vector<Point> pts;
    for (const json &r : rows)
        pts.push_back ({ms ? from_ms (r.at (0)) : from_s (r.at (0)),
                        num (r.at (close_idx))
                       });
    sort_points (pts);
    return pts;
}

const time_zone *
moscow ()
{
    static const time_zone *z = util::zone ("Europe/Moscow");
    return z;
}
}

std::string
Provider::label () const
{
    return type + " " + symbol;
}

const Range &
range_of (Span span)
{
    static const Range ranges[] =
    {
        {Span::week, "week", "7 days", days{7}, hours{1}},
        {Span::month, "month", "1 month", days{30}, hours{6}},
        {Span::half_year, "half-year", "6 months", days{182}, hours{24}},
    };
    return ranges[static_cast<std::size_t> (span)];
}

const Range *
find_range (std::string_view id)
{
    for (Span span :
            {
                Span::week, Span::month, Span::half_year
            })
        if (range_of (span).id == id)
            return &range_of (span);
    return nullptr;
}

std::vector<Point>
parse_binance (std::string_view body)
{
    json j = json::parse (body);
    if (j.is_object ())
        throw std::runtime_error (std::format ("binance: {}",
                                               j.value ("msg", "error")));
    return candles (j, true, 4);
}

std::vector<Point>
parse_bybit (std::string_view body)
{
    json j = json::parse (body);
    if (j.value ("retCode", -1) != 0)
        throw std::runtime_error (std::format ("bybit: {}",
                                               j.value ("retMsg", "error")));
    return candles (j.at ("result").at ("list"), true, 4);
}

std::vector<Point>
parse_okx (std::string_view body)
{
    json j = json::parse (body);
    if (j.value ("code", "") != "0")
        throw std::runtime_error (std::format ("okx: {}",
                                               j.value ("msg", "error")));
    return candles (j.at ("data"), true, 4);
}

std::vector<Point>
parse_coinbase (std::string_view body)
{
    json j = json::parse (body);
    if (j.is_object ())
        throw std::runtime_error (std::format ("coinbase: {}",
                                               j.value ("message", "error")));
    return candles (j, false, 4);
}

std::vector<Point>
parse_kraken (std::string_view body)
{
    json j = json::parse (body);
    if (const json &err = j.at ("error"); !err.empty ())
        throw std::runtime_error (std::format ("kraken: {}",
                                               err.front ().get<std::string> ()));
    for (const auto &[key, rows] : j.at ("result").items ())
        if (key != "last" && rows.is_array ())
            return candles (rows, false, 4);
    throw std::runtime_error ("kraken: no data");
}

std::vector<Point>
parse_coingecko (std::string_view body)
{
    json j = json::parse (body);
    if (auto st = j.find ("status"); st != j.end () && st->is_object ())
        throw std::runtime_error (std::format (
                                      "coingecko: {}", st->value ("error_message", "error")));
    if (auto e = j.find ("error"); e != j.end ())
        throw std::runtime_error (std::format (
                                      "coingecko: {}", e->is_string () ? e->get<std::string> ()
                                      : e->dump ()));
    return candles (j.at ("prices"), true, 1);
}

std::vector<Point>
parse_cbr (std::string_view body)
{
    if (body.find ("<ValCurs") == std::string_view::npos)
        throw std::runtime_error ("cbr: not a ValCurs document");
    auto element = [] (std::string_view rec, std::string_view tag)
    {
        std::string open = std::format ("<{}>", tag);
        std::string close = std::format ("</{}>", tag);
        auto b = rec.find (open);
        auto e = rec.find (close);
        if (b == std::string_view::npos || e == std::string_view::npos || e < b)
            throw std::runtime_error (std::format ("cbr: no <{}>", tag));
        return rec.substr (b + open.size (), e - b - open.size ());
    };
    std::vector<Point> pts;
    std::size_t pos = 0;
    while ((pos = body.find ("<Record ", pos)) != std::string_view::npos)
        {
            auto end = body.find ("</Record>", pos);
            if (end == std::string_view::npos)
                break;
            std::string_view rec = body.substr (pos, end - pos);
            pos = end;

            auto d = rec.find ("Date=\"");
            unsigned D = 0, M = 0;
            int Y = 0;
            if (d == std::string_view::npos
                    || std::sscanf (std::string (rec.substr (d + 6, 10)).c_str (),
                                    "%2u.%2u.%4d", &D, &M, &Y) != 3)
                throw std::runtime_error ("cbr: bad Date");
            year_month_day ymd{year{Y}, month{M}, day{D}};
            if (!ymd.ok ())
                throw std::runtime_error ("cbr: bad Date");

            std::string value (util::trim (element (rec, "Value")));
            std::ranges::replace (value, ',', '.');
            double nominal = to_double (util::trim (element (rec, "Nominal")));
            auto t = moscow ()->to_sys (local_days{ymd});
            pts.push_back ({t, to_double (value) / (nominal > 0 ? nominal : 1)});
        }
    sort_points (pts);
    return pts;
}

std::string
provider_url (const Provider &p, util::TimePoint now, const Range &range)
{
    auto pick = [&range] (std::string_view week, std::string_view month,
                          std::string_view half_year)
    {
        return std::string (range.span == Span::week ? week
                            : range.span == Span::month ? month : half_year);
    };
    const std::string s = http::url_encode (p.symbol);
    const std::string limit = pick ("168", "180", "183");
    if (p.type == "binance")
        return std::format ("https://api.binance.com/api/v3/klines?symbol={}"
                            "&interval={}&limit={}", s, pick ("1h", "4h", "1d"),
                            limit);
    if (p.type == "bybit")
        return std::format ("https://api.bybit.com/v5/market/kline?category=spot"
                            "&symbol={}&interval={}&limit={}", s,
                            pick ("60", "240", "D"), limit);
    if (p.type == "okx")
        return std::format ("https://www.okx.com/api/v5/market/candles?instId={}"
                            "&bar={}&limit={}", s, pick ("1H", "4H", "1Dutc"),
                            limit);
    if (p.type == "coinbase")
        return std::format ("https://api.exchange.coinbase.com/products/{}"
                            "/candles?granularity={}", s,
                            pick ("3600", "21600", "86400"));
    if (p.type == "kraken")
        return std::format ("https://api.kraken.com/0/public/OHLC?pair={}"
                            "&interval={}&since={}", s,
                            pick ("60", "240", "1440"),
                            floor<seconds> (now - range.length)
                            .time_since_epoch ().count ());
    if (p.type == "coingecko")
        return std::format ("https://api.coingecko.com/api/v3/coins/{}"
                            "/market_chart?vs_currency={}&days={}", s,
                            http::url_encode (p.vs), range.length.count ());
    if (p.type == "cbr")
        {
            auto from = year_month_day{floor<days> (util::to_local (
                    now - range.length - days{3}, moscow ()))};
            auto to = year_month_day{floor<days> (util::to_local (now + days{1},
                                                  moscow ()))};
            return std::format ("https://www.cbr.ru/scripts/XML_dynamic.asp"
                                "?date_req1={:%d/%m/%Y}&date_req2={:%d/%m/%Y}"
                                "&VAL_NM_RQ={}", from, to,
                                s.empty () ? "R01235" : s);
        }
    throw std::runtime_error (std::format ("unknown market provider '{}'",
                                           p.type));
}

std::vector<Point>
parse (const Provider &p, std::string_view body)
{
    if (p.type == "binance")
        return parse_binance (body);
    if (p.type == "bybit")
        return parse_bybit (body);
    if (p.type == "okx")
        return parse_okx (body);
    if (p.type == "coinbase")
        return parse_coinbase (body);
    if (p.type == "kraken")
        return parse_kraken (body);
    if (p.type == "coingecko")
        return parse_coingecko (body);
    if (p.type == "cbr")
        return parse_cbr (body);
    throw std::runtime_error (std::format ("unknown market provider '{}'",
                                           p.type));
}

std::vector<Point>
last_span (std::vector<Point> points, days length)
{
    if (!points.empty ())
        {
            util::TimePoint from = points.back ().t - length;
            std::erase_if (points, [from] (const Point &point)
            {
                return point.t < from;
            });
        }
    return points;
}

Result
fetch_chart (const Chart &chart, const Fetcher &fetch, util::TimePoint now)
{
    Result res;
    for (const Provider &p : chart.providers)
        {
            try
                {
                    std::string body = fetch (p);
                    std::vector<Point> pts = last_span (parse (p, body),
                                                        chart.range.length);
                    if (pts.size () < 2)
                        throw std::runtime_error (std::format (
                                                      "no data for the last {}", chart.range.label));
                    if (auto age = now - pts.back ().t;
                            age > chart.max_age + chart.range.bucket)
                        throw std::runtime_error (std::format (
                                                      "stale, last point {} old", util::format_age (pts.back ().t, now)));
                    res.points = std::move (pts);
                    res.provider = p.label ();
                    res.served = p;
                    res.body = std::move (body);
                    return res;
                }
            catch (const std::exception &e)
                {
                    std::string msg = e.what ();
                    if (msg.starts_with ("https://"))
                        if (auto colon = msg.find (": "); colon != std::string::npos)
                            msg = msg.substr (colon + 2);
                    res.failures.push_back (std::format ("{}: {}", p.label (), msg));
                }
        }
    std::string all;
    for (const std::string &f : res.failures)
        all += (all.empty () ? "" : "; ") + f;
    throw std::runtime_error (all.empty () ? "no providers configured" : all);
}

Axis
nice_axis (double lo, double hi, int max_ticks)
{
    Axis a;
    if (!(hi > lo))
        {
            double pad = std::max (std::abs (lo) * 0.01, 1e-9);
            lo -= pad;
            hi += pad;
        }
    double pad = (hi - lo) * 0.04;
    a.lo = lo - pad;
    a.hi = hi + pad;
    max_ticks = std::max (2, max_ticks);
    double rough = (hi - lo) / (max_ticks - 1);
    double mag = std::pow (10.0, std::floor (std::log10 (rough)));
    double norm = rough / mag;
    double nice = norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 2.5 ? 2.5
                  : norm <= 5 ? 5 : 10;
    a.step = nice * mag;
    for (double t = std::ceil (a.lo / a.step) * a.step; t <= a.hi + a.step * 1e-9;
            t += a.step)
        a.ticks.push_back (std::abs (t) < a.step * 1e-9 ? 0.0 : t);
    while (static_cast<int> (a.ticks.size ()) > max_ticks)
        {
            if (a.ticks.front () < lo)
                a.ticks.erase (a.ticks.begin ());
            else
                a.ticks.pop_back ();
        }
    double digits = -std::floor (std::log10 (a.step) + 1e-9);
    a.decimals = std::clamp (static_cast<int> (digits)
                             + (nice == 2.5 ? 1 : 0), 0, 8);
    return a;
}

std::string
format_price (double v, int decimals)
{
    std::string s = std::format ("{:.{}f}", std::abs (v), std::max (0, decimals));
    auto dot = s.find ('.');
    std::size_t int_end = dot == std::string::npos ? s.size () : dot;
    std::string out;
    for (std::size_t i = 0; i < int_end; ++i)
        {
            if (i && (int_end - i) % 3 == 0)
                out += ',';
            out += s[i];
        }
    out += s.substr (int_end);
    return v < 0 ? "-" + out : out;
}

int
auto_decimals (double v)
{
    v = std::abs (v);
    if (v >= 10000)
        return 0;
    if (v >= 1000)
        return 1;
    if (v >= 10)
        return 2;
    if (v >= 1)
        return 3;
    return 5;
}

Statistics
statistics (const std::vector<Point> &points)
{
    Statistics result;
    if (points.empty ())
        return result;
    result.open = points.front ().v;
    result.last = points.back ().v;
    auto [low, high] = std::ranges::minmax (points
                                            | std::views::transform (&Point::v));
    result.low = low;
    result.high = high;
    double sum = 0;
    for (const Point &point : points)
        sum += point.v;
    result.average = sum / static_cast<double> (points.size ());
    auto percent = [] (double from, double to)
    {
        return from != 0 ? (to - from) / from * 100 : 0.0;
    };
    result.change_percent = percent (result.open, result.last);
    result.from_high_percent = percent (result.high, result.last);
    result.from_low_percent = percent (result.low, result.last);

    std::vector<double> returns;
    std::vector<util::TimePoint::duration> gaps;
    for (std::size_t index = 1; index < points.size (); ++index)
        if (points[index - 1].v > 0 && points[index].v > 0)
            {
                returns.push_back (std::log (points[index].v / points[index - 1].v));
                gaps.push_back (points[index].t - points[index - 1].t);
            }
    if (returns.size () >= 2)
        {
            double mean = 0;
            for (double value : returns)
                mean += value;
            mean /= static_cast<double> (returns.size ());
            double variance = 0;
            for (double value : returns)
                variance += (value - mean) * (value - mean);
            variance /= static_cast<double> (returns.size () - 1);
            std::ranges::nth_element (gaps, gaps.begin () + gaps.size () / 2);
            double typical_gap = duration<double> (gaps[gaps.size () / 2]).count ();
            double per_day = typical_gap > 0 ? 86400.0 / typical_gap : 1.0;
            result.daily_volatility_percent = std::sqrt (variance * per_day) * 100;
        }
    return result;
}

std::vector<Chart>
default_charts ()
{
    return
    {
        {
            "XAUT/USDT", "\U000F124F", 1, hours{6}, {
                {"binance", "XAUTUSDT"},
                {"bybit", "XAUTUSDT"},
                {"coingecko", "tether-gold"},
            }, "XAUT · TON", range_of (Span::week), "yellow"
        },
        {
            "TON/USDT", "\U000F01C8", 3, hours{6}, {
                {"kraken", "TONUSD"},
                {"coingecko", "the-open-network"},
                {"binance", "TONUSDT"},
            }, "XAUT · TON", range_of (Span::week), "blue"
        },
        {
            "BTC/USD", "\U000F0813", 0, hours{6}, {
                {"coinbase", "BTC-USD"},
                {"kraken", "XBTUSD"},
                {"binance", "BTCUSDT"},
            }, "BTC · RUB", range_of (Span::week), "green"
        },
        {
            "USD/RUB", "\U000F01B1", 2, hours{100}, {
                {"cbr", "R01235"},
            }, "BTC · RUB", range_of (Span::week), "green"
        },
    };
}

namespace
{
const std::pair<std::string_view, tui::Role> plot_colors[] =
{
    {"yellow", tui::PLOT_YELLOW}, {"gold", tui::PLOT_YELLOW},
    {"green", tui::PLOT_GREEN}, {"blue", tui::PLOT_BLUE},
    {"cyan", tui::PLOT_CYAN}, {"magenta", tui::PLOT_MAGENTA},
    {"red", tui::PLOT_RED}, {"orange", tui::PLOT_ORANGE},
    {"white", tui::PLOT_WHITE},
};
}

bool
known_color (std::string_view name)
{
    return std::ranges::any_of (plot_colors, [name] (const auto &entry)
    {
        return entry.first == name;
    });
}

std::vector<Chart>
expand_ranges (const std::vector<Chart> &instruments,
               const std::vector<Range> &ranges)
{
    std::vector<std::string> screens;
    for (const Chart &instrument : instruments)
        if (std::ranges::find (screens, instrument.screen) == screens.end ())
            screens.push_back (instrument.screen);
    std::vector<Chart> charts;
    for (const std::string &screen : screens)
        for (const Chart &instrument : instruments)
            if (instrument.screen == screen)
                for (const Range &range : ranges)
                    {
                        Chart chart = instrument;
                        chart.range = range;
                        charts.push_back (std::move (chart));
                    }
    return charts;
}

namespace
{
struct ChartState
{
    std::optional<Result> result;
    util::TimePoint fetched{};
    std::string error;
    util::TimePoint error_at{};
};

class MarketsSource : public Source
{
public:
    MarketsSource (std::vector<Chart> charts, seconds interval)
        : Source ("markets", interval), charts_ (std::move (charts))
    {
        state.set (std::vector<ChartState> (charts_.size ()));
    }

    ~MarketsSource () override
    {
        stop ();
    }

    const std::vector<Chart> &charts () const
    {
        return charts_;
    }

    Latest<std::vector<ChartState>> state;

protected:
    void
    fetch () override
    {
        auto now = util::Clock::now ();

        std::vector<ChartState> next = *state.get ();
        std::vector<bool> got (charts_.size (), false);
        {
            std::vector<std::jthread> workers;
            for (std::size_t i = 0; i < charts_.size (); ++i)
                workers.emplace_back ([&, i]
                {
                    ChartState &cs = next[i];
                    const Range &range = charts_[i].range;
                    Fetcher get = [now, &range] (const Provider &provider)
                    {
                        http::Fields headers;
                        headers.emplace_back ("Accept", "application/json, text/xml");
                        return http::get (provider_url (provider, now, range),
                                          std::move (headers));
                    };
                    try
                        {
                            cs.result = fetch_chart (charts_[i], get, now);
                            cs.fetched = util::Clock::now ();
                            cs.error.clear ();
                            got[i] = true;
                        }
                    catch (const std::exception &e)
                        {
                            cs.error = e.what ();
                            cs.error_at = util::Clock::now ();
                        }
                });
        }

        std::string failed;
        for (std::size_t i = 0; i < charts_.size (); ++i)
            if (got[i])
                {
                    const Result &r = *next[i].result;
                    json wrap = {{"type", r.served.type},
                        {"symbol", r.served.symbol},
                        {"vs", r.served.vs},
                        {"body", r.body}
                    };
                    cache_put (cache_key (charts_[i]), wrap.dump ());
                    next[i].result->body.clear ();
                }
        for (std::size_t i = 0; i < charts_.size (); ++i)
            if (!next[i].error.empty ())
                failed += (failed.empty () ? "" : ", ") + charts_[i].name;
        state.set (std::move (next));
        if (!failed.empty ())
            throw std::runtime_error (std::format ("no data for {}", failed));
    }

    std::optional<util::TimePoint>
    restore () override
    {
        std::vector<ChartState> restored (charts_.size ());
        std::optional<util::TimePoint> oldest;
        for (std::size_t i = 0; i < charts_.size (); ++i)
            {
                auto c = cache_get (cache_key (charts_[i]));
                if (!c)
                    return std::nullopt;
                json wrap = json::parse (c->body);
                Provider p{wrap.at ("type").get<std::string> (),
                           wrap.at ("symbol").get<std::string> (),
                           wrap.value ("vs", "usd")};
                if (std::ranges::none_of (charts_[i].providers, [&] (const Provider &q)
                {
                    return q.type == p.type && q.symbol == p.symbol && q.vs == p.vs;
                }))
                return std::nullopt;
                Result r;
                r.points = last_span (parse (p, wrap.at ("body")
                                             .get_ref<const std::string &> ()),
                                      charts_[i].range.length);
                if (r.points.size () < 2)
                    return std::nullopt;
                r.provider = p.label ();
                r.served = std::move (p);
                restored[i].result = std::move (r);
                restored[i].fetched = c->saved;
                if (!oldest || c->saved < *oldest)
                    oldest = c->saved;
            }
        state.set (std::move (restored));
        return oldest;
    }

private:
    static std::string
    cache_key (const Chart &c)
    {
        std::string key = "chart-";
        for (unsigned char ch : c.name)
            if (std::isalnum (ch))
                key += static_cast<char> (std::tolower (ch));
            else if (key.back () != '-')
                key += '-';
        while (key.back () == '-')
            key.pop_back ();
        return key + "-" + c.range.id + ".json";
    }

    std::vector<Chart> charts_;
};

class MarketsScreen : public Screen
{
public:
    MarketsScreen (MarketsSource &src, const time_zone *tz, seconds interval,
                   std::string title, std::vector<std::size_t> chart_indices)
        : src_ (src), tz_ (tz), interval_ (interval), title_ (std::move (title)),
          chart_indices_ (std::move (chart_indices))
    {
    }

    std::string title () const override
    {
        return title_;
    }

    std::vector<Source *> sources () const override
    {
        return {&src_};
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        auto states = src_.state.get ();
        const auto &charts = src_.charts ();
        std::size_t count = chart_indices_.size ();
        if (count == 0)
            return;

        std::vector<std::string> range_ids;
        std::vector<std::string> instruments;
        for (std::size_t index : chart_indices_)
            {
                if (std::ranges::find (range_ids, charts[index].range.id)
                        == range_ids.end ())
                    range_ids.push_back (charts[index].range.id);
                if (std::ranges::find (instruments, charts[index].name)
                        == instruments.end ())
                    instruments.push_back (charts[index].name);
            }
        int column_count = static_cast<int> (range_ids.size ());
        if (column_count < 2)
            column_count = area.w >= 90 && count > 1 ? 2 : 1;
        else if (area.w < 45 * column_count)
            column_count = 1;
        int row_count = static_cast<int> ((count + column_count - 1) / column_count);
        std::vector<tui::Rect> rows = area.rows (row_count, 0);
        static constexpr tui::Role series[] =
        {
            tui::SERIES1, tui::SERIES2, tui::SERIES3, tui::SERIES4
        };
        for (std::size_t position = 0; position < count; ++position)
            {
                const Chart &chart = charts[chart_indices_[position]];
                std::size_t instrument = static_cast<std::size_t> (
                                             std::ranges::find (instruments, chart.name) - instruments.begin ());
                std::vector<tui::Rect> cells
                    = rows[position / column_count].columns (column_count, 0);
                tui::Role color = series[instrument % 4];
                for (const auto &[name, role] : plot_colors)
                    if (name == chart.color)
                        color = role;
                draw_chart (cells[position % column_count], chart,
                            (*states)[chart_indices_[position]], color, now);
            }
    }

private:
    void
    draw_chart (const tui::Rect &cell, const Chart &chart, const ChartState &cs,
                tui::Role color, util::TimePoint now)
    {
        std::string title = chart.name;
        if (tui::nerd_icons () && !chart.icon.empty ())
            title = chart.icon + " " + title;
        attr_t title_attr = 0;
        if (cs.result)
            {
                util::TimePoint last = cs.result->points.back ().t;
                title += std::format (" · {} · {} ago · {}", chart.range.label,
                                      util::format_age (cs.fetched, now),
                                      cs.result->provider);
                if (now - last > chart.max_age + interval_)
                    title_attr = tui::attr (tui::WARN);
            }
        tui::Rect in = tui::frame (cell, title, title_attr);
        if (in.empty ())
            return;

        if (!cs.result)
            {
                if (cs.error.empty ())
                    tui::put (in.y, in.x + 1, "Loading…", tui::attr (tui::DIM));
                else
                    wrap_error (in, cs.error);
                return;
            }

        const std::vector<Point> &pts = cs.result->points;
        double first = pts.front ().v, last = pts.back ().v;
        auto [mn, mx] = std::ranges::minmax (pts | std::views::transform (&Point::v));
        int dec = chart.decimals >= 0 ? chart.decimals : auto_decimals (last);

        double change = last - first;
        double pct = first != 0 ? change / first * 100 : 0;
        bool up = change >= 0;
        int x = in.x + 1;
        x += tui::put (in.y, x, format_price (last, dec),
                       tui::attr (tui::NORMAL) | A_BOLD) + 2;
        std::string arrow = up ? tui::icon ("\U000F0535", "▲")
                            : tui::icon ("\U000F0533", "▼");
        x += tui::put (in.y, x, std::format ("{} {}{} ({}{:.2f}%)", arrow,
                                             up ? "+" : "−", format_price (std::abs (change), dec),
                                             up ? "+" : "−", std::abs (pct)),
                       tui::attr (up ? tui::GOOD : tui::BAD));
        std::string right = std::format ("H {}  L {}", format_price (mx, dec),
                                         format_price (mn, dec));
        tui::Role right_role = tui::DIM;
        if (!cs.error.empty ())
            {
                right = std::format ("✗ refresh failed {} ago",
                                     util::format_age (cs.error_at, now));
                right_role = tui::WARN;
            }
        int rw = tui::width (right);
        if (in.right () - rw - 1 > x + 2)
            tui::put (in.y, in.right () - rw - 1, right, tui::attr (right_role));

        Statistics stats = statistics (pts);
        std::vector<std::string> details =
        {
            std::format ("open {}", format_price (stats.open, dec)),
            std::format ("avg {}", format_price (stats.average, dec)),
            std::format ("{}{:.1f}% from high", stats.from_high_percent < 0 ? "−" : "+",
                         std::abs (stats.from_high_percent)),
            std::format ("{}{:.1f}% from low", stats.from_low_percent < 0 ? "−" : "+",
                         std::abs (stats.from_low_percent)),
            std::format ("volatility {:.2f}%/day", stats.daily_volatility_percent),
        };
        int detail_x = in.x + 1;
        for (const std::string &detail : details)
            {
                int detail_width = tui::width (detail);
                if (detail_x + detail_width > in.right () - 1)
                    break;
                if (detail_x > in.x + 1)
                    detail_x += tui::put (in.y + 1, detail_x, " · ",
                                          tui::attr (tui::FRAME));
                detail_x += tui::put (in.y + 1, detail_x, detail, tui::attr (tui::DIM));
            }

        int plot_h = in.h - 3;
        if (plot_h < 2)
            return;
        Axis axis = nice_axis (mn, mx, std::clamp (plot_h / 2 + 1, 2, 6));
        int label_dec = std::max (axis.decimals, 0);
        int lw = 0;
        for (double t : axis.ticks)
            lw = std::max (lw, tui::width (format_price (t, label_dec)));
        int plot_x = in.x + lw + 2;
        int plot_w = in.right () - plot_x;
        if (plot_w < 4)
            return;
        int top = in.y + 2;

        tui::Braille canvas (plot_w, plot_h);
        auto ydot = [&] (double v)
        {
            double f = (axis.hi - v) / (axis.hi - axis.lo);
            return static_cast<int> (std::lround (f * (canvas.dots_h () - 1)));
        };
        util::TimePoint t1 = pts.back ().t;
        util::TimePoint t0 = t1 - chart.range.length;
        auto xdot = [&] (util::TimePoint t)
        {
            double f = duration<double> (t - t0) / duration<double> (t1 - t0);
            return static_cast<int> (std::lround (std::clamp (f, 0.0, 1.0)
                                                  * (canvas.dots_w () - 1)));
        };

        std::vector<util::TimePoint::duration> gaps;
        for (std::size_t i = 1; i < pts.size (); ++i)
            gaps.push_back (pts[i].t - pts[i - 1].t);
        std::ranges::nth_element (gaps, gaps.begin () + gaps.size () / 2);
        auto max_gap = 4 * gaps[gaps.size () / 2];
        for (std::size_t i = 0; i < pts.size (); ++i)
            {
                int px = xdot (pts[i].t), py = ydot (pts[i].v);
                if (i > 0 && pts[i].t - pts[i - 1].t <= max_gap)
                    canvas.line (xdot (pts[i - 1].t), ydot (pts[i - 1].v), px, py);
                else
                    canvas.set (px, py);
            }

        attr_t fa = tui::attr (tui::FRAME);
        for (int r = 0; r < plot_h; ++r)
            tui::put (top + r, plot_x - 1, "│", fa);
        for (double t : axis.ticks)
            {
                int r = ydot (t) / 4;
                if (r < 0 || r >= plot_h)
                    continue;
                tui::put (top + r, in.x, tui::pad (format_price (t, label_dec), lw, 'r'),
                          tui::attr (tui::DIM));
                tui::put (top + r, plot_x - 1, "┤", fa);
                for (int c = 0; c < plot_w; c += 2)
                    if (!canvas.used (c, r))
                        tui::put (top + r, plot_x + c, "·", tui::attr (tui::FRAME));
            }
        canvas.draw (top, plot_x, tui::attr (color));

        int xl = top + plot_h;
        tui::put (xl, plot_x - 1, "└", fa);
        auto first_day = floor<days> (util::to_local (t0, tz_)) + days{1};
        int next_free = plot_x;
        for (auto midnight = first_day; tz_->to_sys (midnight) <= t1; midnight += days{1})
            {
                year_month_day date{midnight};
                weekday day_of_week{midnight};
                std::string label;
                if (chart.range.span == Span::week)
                    label = std::format ("{:%a %d}", midnight);
                else if (chart.range.span == Span::month)
                    {
                        if (day_of_week != Monday)
                            continue;
                        label = std::format ("{:%d %b}", midnight);
                    }
                else
                    {
                        if (date.day () != day{1})
                            continue;
                        label = std::format ("{:%b}", midnight);
                    }
                int col = plot_x + xdot (tz_->to_sys (midnight)) / 2;
                label = "╵" + label;
                int lwid = tui::width (label);
                if (col < next_free || col + lwid > in.right ())
                    continue;
                tui::put (xl, col, label, tui::attr (tui::DIM));
                next_free = col + lwid + 2;
            }
    }

    static void
    wrap_error (const tui::Rect &in, const std::string &error)
    {
        int y = in.y;
        for (std::string_view part : util::split (error, ';'))
            {
                std::string line (util::trim (part));
                while (!line.empty () && y < in.bottom ())
                    {
                        std::string shown = tui::fit (line, in.w - 2);
                        tui::put (y++, in.x + 1, shown, tui::attr (tui::BAD));
                        if (shown == line)
                            break;
                        std::size_t cut = shown.size () - std::string ("…").size ();
                        line = "  " + line.substr (cut);
                    }
            }
    }

    MarketsSource &src_;
    const time_zone *tz_;
    seconds interval_;
    std::string title_;
    std::vector<std::size_t> chart_indices_;
};

std::vector<Chart>
read_charts (const toml::table &t)
{
    const toml::array *arr = t["charts"].as_array ();
    if (!arr)
        return default_charts ();
    std::vector<Chart> out;
    for (const toml::node &node : *arr)
        {
            const toml::table *ct = node.as_table ();
            if (!ct)
                throw std::runtime_error ("[[markets.charts]] entries must be tables");
            Chart c;
            c.name = (*ct)["name"].value_or<std::string> ("");
            if (c.name.empty ())
                throw std::runtime_error ("[[markets.charts]] needs a name");
            c.icon = (*ct)["icon"].value_or<std::string> ("");
            c.decimals = (*ct)["decimals"].value_or (-1);
            c.max_age = hours ((*ct)["max_age_hours"].value_or (6));
            c.screen = (*ct)["screen"].value_or<std::string> ("Markets");
            c.color = (*ct)["color"].value_or<std::string> ("");
            if (!c.color.empty () && !known_color (c.color))
                throw std::runtime_error (std::format (
                                              "markets chart '{}': unknown color '{}' (yellow, green, blue, "
                                              "cyan, magenta, red, orange, white)", c.name, c.color));
            if (const toml::array *ps = (*ct)["providers"].as_array ())
                for (const toml::node &pn : *ps)
                    {
                        const toml::table *pt = pn.as_table ();
                        if (!pt)
                            throw std::runtime_error (std::format (
                                                          "markets chart '{}': providers must be inline tables", c.name));
                        Provider p;
                        p.type = (*pt)["type"].value_or<std::string> ("");
                        p.symbol = (*pt)["symbol"].value_or<std::string> ("");
                        p.vs = (*pt)["vs"].value_or<std::string> ("usd");
                        provider_url (p, util::Clock::now ());
                        c.providers.push_back (std::move (p));
                    }
            if (c.providers.empty ())
                throw std::runtime_error (std::format (
                                              "markets chart '{}' has no providers", c.name));
            out.push_back (std::move (c));
        }
    return out;
}
}
}

void
install_markets (App &app)
{
    using namespace markets;
    const toml::table &t = app.config ().section ("markets");
    auto interval = seconds (t["interval"].value_or (43200));
    const time_zone *tz = util::zone (
                              t["timezone"].value_or<std::string> ("Europe/Moscow"));
    std::vector<Range> ranges;
    if (const toml::array *ids = t["ranges"].as_array ())
        for (const toml::node &node : *ids)
            {
                std::string id = node.value_or<std::string> ("");
                const Range *range = find_range (id);
                if (!range)
                    throw std::runtime_error (std::format (
                                                  "[markets] ranges: unknown range '{}' (use week, month or "
                                                  "half-year)", id));
                ranges.push_back (*range);
            }
    else
        ranges = {range_of (Span::month), range_of (Span::half_year)};
    if (ranges.empty ())
        throw std::runtime_error ("[markets] ranges is empty");

    std::vector<Chart> charts = expand_ranges (read_charts (t), ranges);
    std::vector<std::string> titles;
    std::vector<std::vector<std::size_t>> members;
    for (std::size_t index = 0; index < charts.size (); ++index)
        {
            auto found = std::ranges::find (titles, charts[index].screen);
            if (found == titles.end ())
                {
                    titles.push_back (charts[index].screen);
                    members.emplace_back ();
                    found = titles.end () - 1;
                }
            members[static_cast<std::size_t> (found - titles.begin ())].push_back (index);
        }
    auto &src = app.add_source (std::make_unique<MarketsSource> (
                                    std::move (charts), interval));
    for (std::size_t screen = 0; screen < titles.size (); ++screen)
        app.add_screen (std::make_unique<MarketsScreen> (src, tz, interval,
                        titles[screen], members[screen]));
}
