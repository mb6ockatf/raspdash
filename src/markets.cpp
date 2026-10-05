/* markets.cpp -- week-long price charts.

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
provider_url (const Provider &p, util::TimePoint now)
{
    const std::string s = http::url_encode (p.symbol);
    if (p.type == "binance")
        return std::format ("https://api.binance.com/api/v3/klines?symbol={}"
                            "&interval=1h&limit=168", s);
    if (p.type == "bybit")
        return std::format ("https://api.bybit.com/v5/market/kline?category=spot"
                            "&symbol={}&interval=60&limit=168", s);
    if (p.type == "okx")
        return std::format ("https://www.okx.com/api/v5/market/candles?instId={}"
                            "&bar=1H&limit=168", s);
    if (p.type == "coinbase")
        return std::format ("https://api.exchange.coinbase.com/products/{}"
                            "/candles?granularity=3600", s);
    if (p.type == "kraken")
        return std::format ("https://api.kraken.com/0/public/OHLC?pair={}"
                            "&interval=60&since={}", s,
                            floor<seconds> (now - days{7})
                            .time_since_epoch ().count ());
    if (p.type == "coingecko")
        return std::format ("https://api.coingecko.com/api/v3/coins/{}"
                            "/market_chart?vs_currency={}&days=7", s,
                            http::url_encode (p.vs));
    if (p.type == "cbr")
        {
            auto from = year_month_day{floor<days> (util::to_local (now - days{10},
                                                    moscow ()))};
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
last_week (std::vector<Point> pts)
{
    if (!pts.empty ())
        {
            util::TimePoint from = pts.back ().t - days{7};
            std::erase_if (pts, [from] (const Point &pt)
            {
                return pt.t < from;
            });
        }
    return pts;
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
                    std::vector<Point> pts = last_week (parse (p, body));
                    if (pts.size () < 2)
                        throw std::runtime_error ("no data for the last week");
                    if (auto age = now - pts.back ().t; age > chart.max_age)
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

std::vector<Chart>
default_charts ()
{
    return
    {
        {
            "TON/USDT", "\U000F01C8", 3, hours{6}, {
                {"kraken", "TONUSD"},
                {"coingecko", "the-open-network"},
                {"binance", "TONUSDT"},
            }
        },
        {
            "XAUT/USDT", "\U000F124F", 1, hours{6}, {
                {"binance", "XAUTUSDT"},
                {"bybit", "XAUTUSDT"},
                {"coingecko", "tether-gold"},
            }
        },
        {
            "USD/RUB", "\U000F01B1", 2, hours{100}, {
                {"cbr", "R01235"},
            }
        },
        {
            "BTC/USD", "\U000F0813", 0, hours{6}, {
                {"coinbase", "BTC-USD"},
                {"kraken", "XBTUSD"},
                {"binance", "BTCUSDT"},
            }
        },
    };
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
        Fetcher get = [now] (const Provider &p)
        {
            http::Fields headers;
            headers.emplace_back ("Accept", "application/json, text/xml");
            return http::get (provider_url (p, now), std::move (headers));
        };

        std::vector<ChartState> next = *state.get ();
        std::vector<bool> got (charts_.size (), false);
        {
            std::vector<std::jthread> workers;
            for (std::size_t i = 0; i < charts_.size (); ++i)
                workers.emplace_back ([&, i]
                {
                    ChartState &cs = next[i];
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
                r.points = last_week (parse (p, wrap.at ("body")
                                             .get_ref<const std::string &> ()));
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
        return key + ".json";
    }

    std::vector<Chart> charts_;
};

class MarketsScreen : public Screen
{
public:
    MarketsScreen (MarketsSource &src, const time_zone *tz, seconds interval)
        : src_ (src), tz_ (tz), interval_ (interval)
    {
    }

    std::string title () const override
    {
        return "Markets";
    }

    std::vector<Source *> sources () const override
    {
        return {&src_};
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        auto st = src_.state.get ();
        const auto &charts = src_.charts ();
        std::size_t n = charts.size ();
        if (n == 0)
            return;

        int ncols = area.w >= 90 && n > 1 ? 2 : 1;
        int nrows = static_cast<int> ((n + ncols - 1) / ncols);
        std::vector<tui::Rect> rows = area.rows (nrows, 0);
        static constexpr tui::Role series[] =
        {
            tui::SERIES1, tui::SERIES2, tui::SERIES3, tui::SERIES4
        };
        for (std::size_t i = 0; i < n; ++i)
            {
                std::vector<tui::Rect> cells = rows[i / ncols].columns (ncols, 0);
                draw_chart (cells[i % ncols], charts[i], (*st)[i],
                            series[i % 4], now);
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
                title += std::format (" · {} ago · {}",
                                      util::format_age (last, now),
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

        int plot_h = in.h - 2;
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
        int top = in.y + 1;

        tui::Braille canvas (plot_w, plot_h);
        auto ydot = [&] (double v)
        {
            double f = (axis.hi - v) / (axis.hi - axis.lo);
            return static_cast<int> (std::lround (f * (canvas.dots_h () - 1)));
        };
        util::TimePoint t1 = pts.back ().t;
        util::TimePoint t0 = t1 - days{7};
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
        auto day0 = floor<days> (util::to_local (t0, tz_)) + days{1};
        int next_free = plot_x;
        for (auto d = day0; tz_->to_sys (d) <= t1; d += days{1})
            {
                int col = plot_x + xdot (tz_->to_sys (d)) / 2;
                std::string label = std::format ("{:%a %d}", d);
                int lwid = tui::width (label);
                if (col < next_free || col + lwid > in.right ())
                    continue;
                tui::put (xl, col, "╵" + label.substr (0), tui::attr (tui::DIM));
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
    auto &src = app.add_source (std::make_unique<MarketsSource> (
                                    read_charts (t), interval));
    app.add_screen (std::make_unique<MarketsScreen> (src, tz, interval));
}
