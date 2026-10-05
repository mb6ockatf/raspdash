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

struct Columns
{
    std::size_t open, high, low, close;
};

constexpr Columns ohlc_columns{1, 2, 3, 4};

std::vector<Point>
candles (const json &rows, bool ms, Columns columns)
{
    std::vector<Point> points;
    for (const json &row : rows)
        {
            Point point;
            point.t = ms ? from_ms (row.at (0)) : from_s (row.at (0));
            point.v = num (row.at (columns.close));
            point.open = num (row.at (columns.open));
            point.high = num (row.at (columns.high));
            point.low = num (row.at (columns.low));
            points.push_back (point);
        }
    sort_points (points);
    return points;
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
        {
            Span::week, "week", "7 days", days{7}, hours{1}, days{10}, hours{24},
            hours{72}
        },
        {
            Span::month, "month", "1 month", days{30}, hours{6}, days{40},
            hours{5 * 24}, hours{10 * 24}
        },
        {
            Span::half_year, "half-year", "6 months", days{182}, hours{24},
            days{240}, hours{20 * 24}, hours{50 * 24}
        },
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
    return candles (j, true, ohlc_columns);
}

std::vector<Point>
parse_bybit (std::string_view body)
{
    json j = json::parse (body);
    if (j.value ("retCode", -1) != 0)
        throw std::runtime_error (std::format ("bybit: {}",
                                               j.value ("retMsg", "error")));
    return candles (j.at ("result").at ("list"), true, ohlc_columns);
}

std::vector<Point>
parse_okx (std::string_view body)
{
    json j = json::parse (body);
    if (j.value ("code", "") != "0")
        throw std::runtime_error (std::format ("okx: {}",
                                               j.value ("msg", "error")));
    return candles (j.at ("data"), true, ohlc_columns);
}

std::vector<Point>
parse_coinbase (std::string_view body)
{
    json j = json::parse (body);
    if (j.is_object ())
        throw std::runtime_error (std::format ("coinbase: {}",
                                               j.value ("message", "error")));
    return candles (j, false, Columns{3, 2, 1, 4});
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
            return candles (rows, false, ohlc_columns);
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
    return candles (j.at ("prices"), true, Columns{1, 1, 1, 1});
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
            double rate = to_double (value) / (nominal > 0 ? nominal : 1);
            pts.push_back ({t, rate, rate, rate, rate});
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
    const std::string limit = "240";
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
                            floor<seconds> (now - range.history)
                            .time_since_epoch ().count ());
    if (p.type == "coingecko")
        return std::format ("https://api.coingecko.com/api/v3/coins/{}"
                            "/market_chart?vs_currency={}&days={}", s,
                            http::url_encode (p.vs), range.history.count ());
    if (p.type == "cbr")
        {
            auto from = year_month_day{floor<days> (util::to_local (
                    now - range.history - days{3}, moscow ()))};
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
                    std::vector<Point> history = last_span (parse (p, body),
                                                            chart.range.history);
                    std::vector<Point> pts = last_span (history, chart.range.length);
                    if (pts.size () < 2)
                        throw std::runtime_error (std::format (
                                                      "no data for the last {}", chart.range.label));
                    if (auto age = now - pts.back ().t;
                            age > chart.max_age + chart.range.bucket)
                        throw std::runtime_error (std::format (
                                                      "stale, last point {} old", util::format_age (pts.back ().t, now)));
                    res.points = std::move (pts);
                    res.history = std::move (history);
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

std::vector<Point>
moving_average (const std::vector<Point> &points, hours window)
{
    std::vector<Point> averaged;
    if (points.empty ())
        return averaged;
    double sum = 0;
    std::size_t oldest = 0;
    for (std::size_t index = 0; index < points.size (); ++index)
        {
            sum += points[index].v;
            while (points[oldest].t <= points[index].t - window)
                sum -= points[oldest++].v;
            if (points[index].t - window >= points.front ().t)
                {
                    double value = sum / static_cast<double> (index - oldest + 1);
                    averaged.push_back ({points[index].t, value, value, value, value});
                }
        }
    return averaged;
}

std::optional<double>
change_over (const std::vector<Point> &points, hours period, hours tolerance)
{
    if (points.size () < 2)
        return std::nullopt;
    util::TimePoint target = points.back ().t - period;
    if (points.front ().t > target + tolerance)
        return std::nullopt;
    const Point *base = &points.front ();
    for (const Point &point : points)
        if (point.t <= target)
            base = &point;
        else
            break;
    if (base->v == 0)
        return std::nullopt;
    return (points.back ().v - base->v) / base->v * 100;
}

std::vector<Candle>
make_candles (const std::vector<Point> &points, minutes size,
              const time_zone *zone)
{
    std::vector<Candle> result;
    long long current_key = 0;
    for (const Point &point : points)
        {
            bool has_range = point.high != 0 || point.low != 0;
            double open = has_range ? point.open : point.v;
            double high = has_range ? point.high : point.v;
            double low = has_range ? point.low : point.v;
            auto local = util::to_local (point.t, zone);
            long long key = floor<minutes> (local.time_since_epoch ()).count ()
                            / size.count ();
            if (result.empty () || key != current_key)
                {
                    current_key = key;
                    local_seconds start{minutes{key * size.count ()}};
                    result.push_back ({zone->to_sys (start, choose::earliest),
                                       open, high, low, point.v
                                      });
                }
            else
                {
                    Candle &candle = result.back ();
                    candle.high = std::max (candle.high, high);
                    candle.low = std::min (candle.low, low);
                    candle.close = point.v;
                }
        }
    return result;
}

minutes
candle_size (days length, int max_candles)
{
    static constexpr int sizes[] = {60, 120, 240, 360, 480, 720, 1440, 2880, 4320,
                                    10080
                                   };
    for (int size : sizes)
        if (duration_cast<minutes> (length).count () / size <= max_candles)
            return minutes{size};
    return minutes{sizes[std::size (sizes) - 1]};
}

std::string
format_hours (hours span)
{
    if (span.count () % 24 == 0)
        return std::format ("{}d", span.count () / 24);
    return std::format ("{}h", span.count ());
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
                r.history = last_span (parse (p, wrap.at ("body")
                                              .get_ref<const std::string &> ()),
                                       charts_[i].range.history);
                r.points = last_span (r.history, charts_[i].range.length);
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

struct PlotOptions
{
    bool candles = true;
    bool averages = true;
};

tui::Role
plot_role (const Chart &chart, std::size_t instrument_index)
{
    static constexpr tui::Role series[] =
    {
        tui::SERIES1, tui::SERIES2, tui::SERIES3, tui::SERIES4
    };
    for (const auto &[name, role] : plot_colors)
        if (name == chart.color)
            return role;
    return series[instrument_index % 4];
}

std::string
signed_percent (double value, int decimals = 1)
{
    return std::format ("{}{:.{}f}%", value < 0 ? "−" : "+", std::abs (value), decimals);
}

void
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

struct PlotArea
{
    int top = 0;
    int left = 0;
    int width = 0;
    int height = 0;
    Axis axis;
    util::TimePoint start{};
    util::TimePoint end{};

    int
    dot_y (double value) const
    {
        double fraction = (axis.hi - value) / (axis.hi - axis.lo);
        return static_cast<int> (std::lround (fraction * (height * 4 - 1)));
    }

    double
    half_rows (double value) const
    {
        double fraction = (axis.hi - value) / (axis.hi - axis.lo);
        return fraction * (height * 2 - 1);
    }

    int
    dot_x (util::TimePoint when) const
    {
        double fraction = duration<double> (when - start) / duration<double> (end - start);
        return static_cast<int> (std::lround (std::clamp (fraction, 0.0, 1.0)
                                              * (width * 2 - 1)));
    }
};

std::optional<PlotArea>
prepare_plot (const tui::Rect &in, int top, double low, double high,
              util::TimePoint start, util::TimePoint end,
              const std::function<std::string (double, int)> &label_of)
{
    PlotArea plot;
    plot.top = top;
    plot.height = in.bottom () - top - 1;
    if (plot.height < 2)
        return std::nullopt;
    plot.axis = nice_axis (low, high, std::clamp (plot.height / 2 + 1, 2, 6));
    int label_decimals = std::max (plot.axis.decimals, 0);
    int label_width = 0;
    for (double tick : plot.axis.ticks)
        label_width = std::max (label_width, tui::width (label_of (tick, label_decimals)));
    plot.left = in.x + label_width + 2;
    plot.width = in.right () - plot.left;
    if (plot.width < 4)
        return std::nullopt;
    plot.start = start;
    plot.end = end;

    attr_t frame_attr = tui::attr (tui::FRAME);
    for (int row = 0; row < plot.height; ++row)
        tui::put (top + row, plot.left - 1, "│", frame_attr);
    for (double tick : plot.axis.ticks)
        {
            int row = plot.dot_y (tick) / 4;
            if (row < 0 || row >= plot.height)
                continue;
            tui::put (top + row, in.x, tui::pad (label_of (tick, label_decimals),
                                                 label_width, 'r'), tui::attr (tui::DIM));
            tui::put (top + row, plot.left - 1, "┤", frame_attr);
            for (int column = 0; column < plot.width; column += 2)
                tui::put (top + row, plot.left + column, "·", frame_attr);
        }
    return plot;
}

void
draw_time_axis (const PlotArea &plot, int right, Span span, const time_zone *zone)
{
    int axis_row = plot.top + plot.height;
    tui::put (axis_row, plot.left - 1, "└", tui::attr (tui::FRAME));
    auto first_day = floor<days> (util::to_local (plot.start, zone)) + days{1};
    int next_free = plot.left;
    for (auto midnight = first_day; zone->to_sys (midnight) <= plot.end;
            midnight += days{1})
        {
            year_month_day date{midnight};
            weekday day_of_week{midnight};
            std::string label;
            if (span == Span::week)
                label = std::format ("{:%a %d}", midnight);
            else if (span == Span::month)
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
            int column = plot.left + plot.dot_x (zone->to_sys (midnight)) / 2;
            label = "╵" + label;
            int label_width = tui::width (label);
            if (column < next_free || column + label_width > right)
                continue;
            tui::put (axis_row, column, label, tui::attr (tui::DIM));
            next_free = column + label_width + 2;
        }
}

void
trace_line (tui::Braille &canvas, const PlotArea &plot, const std::vector<Point> &points,
            const std::function<double (const Point &)> &value_of)
{
    if (points.empty ())
        return;
    std::vector<util::TimePoint::duration> gaps;
    for (std::size_t index = 1; index < points.size (); ++index)
        gaps.push_back (points[index].t - points[index - 1].t);
    util::TimePoint::duration max_gap = hours{24 * 365};
    if (!gaps.empty ())
        {
            std::ranges::nth_element (gaps, gaps.begin () + gaps.size () / 2);
            max_gap = 4 * gaps[gaps.size () / 2];
        }
    for (std::size_t index = 0; index < points.size (); ++index)
        {
            if (points[index].t < plot.start)
                continue;
            int column = plot.dot_x (points[index].t);
            int row = plot.dot_y (value_of (points[index]));
            if (index > 0 && points[index - 1].t >= plot.start
                    && points[index].t - points[index - 1].t <= max_gap)
                canvas.line (plot.dot_x (points[index - 1].t),
                             plot.dot_y (value_of (points[index - 1])), column, row);
            else
                canvas.set (column, row);
        }
}

void
draw_candles (const PlotArea &plot, const std::vector<Candle> &candles,
              minutes size)
{
    for (const Candle &candle : candles)
        {
            util::TimePoint middle = candle.start + size / 2;
            if (middle < plot.start)
                middle = plot.start;
            int column = plot.left + plot.dot_x (middle) / 2;
            bool rising = candle.close >= candle.open;
            attr_t candle_attr = tui::attr (rising ? tui::GOOD : tui::BAD);
            double body_top = plot.half_rows (std::max (candle.open, candle.close));
            double body_bottom = plot.half_rows (std::min (candle.open, candle.close));
            double wick_top = plot.half_rows (candle.high);
            double wick_bottom = plot.half_rows (candle.low);
            for (int row = 0; row < plot.height; ++row)
                {
                    auto in_range = [] (int half, double from, double to)
                    {
                        return half >= std::floor (from + 0.5) && half <= std::floor (to + 0.5);
                    };
                    int upper = row * 2;
                    int lower = row * 2 + 1;
                    bool upper_body = in_range (upper, body_top, body_bottom);
                    bool lower_body = in_range (lower, body_top, body_bottom);
                    bool upper_wick = in_range (upper, wick_top, wick_bottom);
                    bool lower_wick = in_range (lower, wick_top, wick_bottom);
                    const char *glyph = nullptr;
                    if (upper_body && lower_body)
                        glyph = "█";
                    else if (upper_body)
                        glyph = lower_wick ? "█" : "▀";
                    else if (lower_body)
                        glyph = upper_wick ? "█" : "▄";
                    else if (upper_wick && lower_wick)
                        glyph = "│";
                    else if (upper_wick)
                        glyph = "╵";
                    else if (lower_wick)
                        glyph = "╷";
                    if (glyph)
                        tui::put (plot.top + row, column, glyph, candle_attr);
                }
        }
}

std::vector<std::pair<std::string, std::optional<double>>>
instrument_changes (const std::string &name, const std::vector<Chart> &charts,
                    const std::vector<ChartState> &states)
{
    std::vector<std::size_t> owners;
    for (std::size_t index = 0; index < charts.size (); ++index)
        if (charts[index].name == name && states[index].result)
            owners.push_back (index);
    std::ranges::sort (owners, {}, [&charts] (std::size_t index)
    {
        return charts[index].range.length;
    });
    static const std::pair<const char *, hours> periods[] =
    {
        {"24h", hours{24}}, {"7d", hours{7 * 24}}, {"30d", hours{30 * 24}},
        {"6m", hours{182 * 24}},
    };
    std::vector<std::pair<std::string, std::optional<double>>> changes;
    for (const auto &[label, period] : periods)
        {
            std::optional<double> change;
            for (std::size_t index : owners)
                {
                    change = change_over (states[index].result->history, period,
                                          charts[index].range.bucket);
                    if (change)
                        break;
                }
            changes.emplace_back (label, change);
        }
    return changes;
}

void
put_items (int row, int left, int right,
           const std::vector<std::pair<std::string, tui::Role>> &items)
{
    int column = left;
    for (const auto &[text, role] : items)
        {
            int item_width = tui::width (text);
            int separator = column > left ? 3 : 0;
            if (column + separator + item_width > right)
                break;
            if (separator)
                column += tui::put (row, column, " · ", tui::attr (tui::FRAME));
            column += tui::put (row, column, text, tui::attr (role));
        }
}

class MarketsScreen : public Screen
{
public:
    MarketsScreen (MarketsSource &src, const time_zone *tz, seconds interval,
                   std::string title, std::vector<std::size_t> chart_indices,
                   PlotOptions options)
        : src_ (src), tz_ (tz), interval_ (interval), title_ (std::move (title)),
          chart_indices_ (std::move (chart_indices)), options_ (options)
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
        for (const Chart &chart : charts)
            if (std::ranges::find (instruments, chart.name) == instruments.end ())
                instruments.push_back (chart.name);
        for (std::size_t index : chart_indices_)
            if (std::ranges::find (range_ids, charts[index].range.id) == range_ids.end ())
                range_ids.push_back (charts[index].range.id);
        int column_count = static_cast<int> (range_ids.size ());
        if (column_count < 2)
            column_count = area.w >= 90 && count > 1 ? 2 : 1;
        else if (area.w < 45 * column_count)
            column_count = 1;
        int row_count = static_cast<int> ((count + column_count - 1) / column_count);
        std::vector<tui::Rect> rows = area.rows (row_count, 0);
        for (std::size_t position = 0; position < count; ++position)
            {
                const Chart &chart = charts[chart_indices_[position]];
                std::size_t instrument = static_cast<std::size_t> (
                                             std::ranges::find (instruments, chart.name) - instruments.begin ());
                std::vector<tui::Rect> cells
                    = rows[position / column_count].columns (column_count, 0);
                draw_chart (cells[position % column_count], chart,
                            (*states)[chart_indices_[position]],
                            instrument_changes (chart.name, charts, *states),
                            plot_role (chart, instrument), now);
            }
    }

private:
    void
    draw_chart (const tui::Rect &cell, const Chart &chart, const ChartState &cs,
                const std::vector<std::pair<std::string, std::optional<double>>> &changes,
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

        const std::vector<Point> &points = cs.result->points;
        const std::vector<Point> &history = cs.result->history;
        Statistics stats = statistics (points);
        int decimals = chart.decimals >= 0 ? chart.decimals : auto_decimals (stats.last);

        double change = stats.last - stats.open;
        bool rising = change >= 0;
        int column = in.x + 1;
        column += tui::put (in.y, column, format_price (stats.last, decimals),
                            tui::attr (tui::NORMAL) | A_BOLD) + 2;
        std::string arrow = rising ? tui::icon ("\U000F0535", "▲")
                            : tui::icon ("\U000F0533", "▼");
        column += tui::put (in.y, column, std::format ("{} {}{} ({})", arrow,
                            rising ? "+" : "−", format_price (std::abs (change), decimals),
                            signed_percent (stats.change_percent, 2)),
                            tui::attr (rising ? tui::GOOD : tui::BAD));
        std::string right = std::format ("H {}  L {}", format_price (stats.high, decimals),
                                         format_price (stats.low, decimals));
        tui::Role right_role = tui::DIM;
        if (!cs.error.empty ())
            {
                right = std::format ("✗ refresh failed {} ago",
                                     util::format_age (cs.error_at, now));
                right_role = tui::WARN;
            }
        int right_width = tui::width (right);
        if (in.right () - right_width - 1 > column + 2)
            tui::put (in.y, in.right () - right_width - 1, right, tui::attr (right_role));

        std::vector<std::pair<std::string, tui::Role>> change_items;
        for (const auto &[label, value] : changes)
            change_items.emplace_back (std::format ("{} {}", label,
                                                    value ? signed_percent (*value, std::abs (*value) < 1 ? 2 : 1)
                                                    : std::string ("—")),
                                       !value ? tui::DIM : *value >= 0 ? tui::GOOD : tui::BAD);
        put_items (in.y + 1, in.x + 1, in.right () - 1, change_items);

        std::vector<Point> fast;
        std::vector<Point> slow;
        if (options_.averages)
            {
                fast = moving_average (history, chart.range.fast_average);
                slow = moving_average (history, chart.range.slow_average);
            }
        std::vector<std::pair<std::string, tui::Role>> details =
        {
            {std::format ("open {}", format_price (stats.open, decimals)), tui::DIM},
            {std::format ("avg {}", format_price (stats.average, decimals)), tui::DIM},
            {std::format ("{} from high", signed_percent (stats.from_high_percent)), tui::DIM},
            {std::format ("{} from low", signed_percent (stats.from_low_percent)), tui::DIM},
            {std::format ("vol {:.2f}%/d", stats.daily_volatility_percent), tui::DIM},
        };
        if (options_.averages)
            {
                details.emplace_back (std::format ("MA{} ━", format_hours (chart.range.fast_average)),
                                      tui::PLOT_WHITE);
                details.emplace_back (std::format ("MA{} ━", format_hours (chart.range.slow_average)),
                                      tui::PLOT_MAGENTA);
            }
        put_items (in.y + 2, in.x + 1, in.right () - 1, details);

        double low = stats.low;
        double high = stats.high;
        minutes size{0};
        std::vector<Candle> candles;
        util::TimePoint end = points.back ().t;
        util::TimePoint start = end - chart.range.length;
        if (options_.candles)
            {
                int estimated_width = std::max (8, in.w - 10);
                size = candle_size (chart.range.length, estimated_width / 2);
                candles = make_candles (points, size, tz_);
                for (const Candle &candle : candles)
                    {
                        low = std::min (low, candle.low);
                        high = std::max (high, candle.high);
                    }
                end = candles.back ().start + size;
                start = end - chart.range.length;
            }
        for (const std::vector<Point> *average :
                {
                    &fast, &slow
                })
            for (const Point &point : *average)
                if (point.t >= start)
                    {
                        low = std::min (low, point.v);
                        high = std::max (high, point.v);
                    }

        auto label_of = [] (double value, int label_decimals)
        {
            return format_price (value, label_decimals);
        };
        std::optional<PlotArea> plot = prepare_plot (in, in.y + 3, low, high, start, end,
                                       label_of);
        if (!plot)
            return;
        auto close_of = [] (const Point &point)
        {
            return point.v;
        };
        tui::Braille slow_canvas (plot->width, plot->height);
        tui::Braille fast_canvas (plot->width, plot->height);
        trace_line (slow_canvas, *plot, slow, close_of);
        trace_line (fast_canvas, *plot, fast, close_of);
        slow_canvas.draw (plot->top, plot->left, tui::attr (tui::PLOT_MAGENTA));
        fast_canvas.draw (plot->top, plot->left, tui::attr (tui::PLOT_WHITE));
        if (options_.candles)
            draw_candles (*plot, candles, size);
        else
            {
                tui::Braille price_canvas (plot->width, plot->height);
                trace_line (price_canvas, *plot, points, close_of);
                price_canvas.draw (plot->top, plot->left, tui::attr (color));
            }
        draw_time_axis (*plot, in.right (), chart.range.span, tz_);
    }

    MarketsSource &src_;
    const time_zone *tz_;
    seconds interval_;
    std::string title_;
    std::vector<std::size_t> chart_indices_;
    PlotOptions options_;
};

class CompareScreen : public Screen
{
public:
    CompareScreen (MarketsSource &src, const time_zone *tz)
        : src_ (src), tz_ (tz)
    {
    }

    std::string title () const override
    {
        return "Compare";
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
        std::vector<std::string> instruments;
        std::vector<Range> ranges;
        for (const Chart &chart : charts)
            {
                if (std::ranges::find (instruments, chart.name) == instruments.end ())
                    instruments.push_back (chart.name);
                if (std::ranges::find (ranges, chart.range.id, &Range::id) == ranges.end ())
                    ranges.push_back (chart.range);
            }
        if (ranges.empty ())
            return;
        int column_count = area.w >= 45 * static_cast<int> (ranges.size ())
                           ? static_cast<int> (ranges.size ()) : 1;
        int row_count = (static_cast<int> (ranges.size ()) + column_count - 1) / column_count;
        std::vector<tui::Rect> rows = area.rows (row_count, 0);
        for (std::size_t position = 0; position < ranges.size (); ++position)
            {
                std::vector<tui::Rect> cells
                    = rows[position / column_count].columns (column_count, 0);
                draw_panel (cells[position % column_count], ranges[position], charts,
                            *states, instruments, now);
            }
    }

private:
    void
    draw_panel (const tui::Rect &cell, const Range &range, const std::vector<Chart> &charts,
                const std::vector<ChartState> &states,
                const std::vector<std::string> &instruments, util::TimePoint now)
    {
        tui::Rect in = tui::frame (cell, std::format ("{} Performance · {} · rebased to 0%",
                                   tui::icon ("\U000F0128", "≋"), range.label));
        if (in.empty ())
            return;

        struct Series
        {
            std::vector<Point> points;
            tui::Role role;
            std::string name;
        };
        std::vector<Series> series;
        util::TimePoint end{};
        for (std::size_t index = 0; index < charts.size (); ++index)
            {
                const Chart &chart = charts[index];
                if (chart.range.id != range.id || !states[index].result)
                    continue;
                const std::vector<Point> &points = states[index].result->points;
                if (points.empty () || points.front ().v == 0)
                    continue;
                Series entry;
                entry.name = chart.name;
                std::size_t instrument = static_cast<std::size_t> (
                                             std::ranges::find (instruments, chart.name) - instruments.begin ());
                entry.role = plot_role (chart, instrument);
                for (const Point &point : points)
                    {
                        double percent = (point.v / points.front ().v - 1) * 100;
                        entry.points.push_back ({point.t, percent, percent, percent, percent});
                    }
                end = std::max (end, points.back ().t);
                series.push_back (std::move (entry));
            }
        if (series.empty ())
            {
                tui::put (in.y, in.x + 1, "Loading…", tui::attr (tui::DIM));
                return;
            }
        std::ranges::sort (series, std::greater<> (), [] (const Series &entry)
        {
            return entry.points.back ().v;
        });

        std::vector<std::pair<std::string, tui::Role>> legend;
        double low = 0;
        double high = 0;
        for (const Series &entry : series)
            {
                legend.emplace_back (std::format ("━ {} {}", entry.name,
                                                  signed_percent (entry.points.back ().v)), entry.role);
                for (const Point &point : entry.points)
                    {
                        low = std::min (low, point.v);
                        high = std::max (high, point.v);
                    }
            }
        put_items (in.y, in.x + 1, in.right () - 1, legend);
        (void) now;

        auto label_of = [] (double value, int label_decimals)
        {
            return std::format ("{}{:.{}f}%", value > 0 ? "+" : value < 0 ? "−" : "",
                                std::abs (value), label_decimals);
        };
        std::optional<PlotArea> plot = prepare_plot (in, in.y + 1, low, high,
                                       end - range.length, end, label_of);
        if (!plot)
            return;
        auto value_of = [] (const Point &point)
        {
            return point.v;
        };
        int zero_row = plot->dot_y (0) / 4;
        if (zero_row >= 0 && zero_row < plot->height)
            for (int column = 0; column < plot->width; ++column)
                tui::put (plot->top + zero_row, plot->left + column, "─",
                          tui::attr (tui::FRAME));
        for (auto entry = series.rbegin (); entry != series.rend (); ++entry)
            {
                tui::Braille canvas (plot->width, plot->height);
                trace_line (canvas, *plot, entry->points, value_of);
                canvas.draw (plot->top, plot->left, tui::attr (entry->role));
            }
        draw_time_axis (*plot, in.right (), range.span, tz_);
    }

    MarketsSource &src_;
    const time_zone *tz_;
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
    PlotOptions options;
    std::string style = t["style"].value_or<std::string> ("candles");
    if (style != "candles" && style != "line")
        throw std::runtime_error (std::format (
                                      "[markets] style: unknown style '{}' (candles or line)", style));
    options.candles = style == "candles";
    options.averages = t["moving_averages"].value_or (true);
    for (std::size_t screen = 0; screen < titles.size (); ++screen)
        app.add_screen (std::make_unique<MarketsScreen> (src, tz, interval,
                        titles[screen], members[screen], options));
    if (t["compare"].value_or (true))
        app.add_screen (std::make_unique<CompareScreen> (src, tz));
}
