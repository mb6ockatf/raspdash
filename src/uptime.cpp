/* uptime.cpp -- boot history of the machines and power-cut detection.

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

#include "uptime.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "app.hpp"
#include "machines.hpp"
#include "modules.hpp"

using json = nlohmann::json;
using namespace std::chrono;

namespace uptime
{
namespace
{
constexpr double same_boot_slack_s = 120;

util::TimePoint
boot_time (const Observation &observation)
{
    auto uptime = duration_cast<util::Clock::duration> (
                      duration<double> (observation.uptime_s));
    return floor<seconds> (observation.taken - uptime);
}

seconds
gap (util::TimePoint first, util::TimePoint second)
{
    return duration_cast<seconds> (first > second ? first - second
                                   : second - first);
}

void
normalize (HostHistory &host, const Settings &settings)
{
    std::ranges::sort (host.boots);
    std::vector<util::TimePoint> merged;
    for (util::TimePoint boot : host.boots)
        if (merged.empty () || gap (merged.back (), boot) > settings.same_boot)
            merged.push_back (boot);
    if (merged.size () > settings.keep)
        merged.erase (merged.begin (),
                      merged.end () - static_cast<std::ptrdiff_t> (settings.keep));
    host.boots = std::move (merged);
}

bool
is_router (std::string_view kind)
{
    return kind == "keenetic";
}
}

bool
observe (HostHistory &host, const Observation &observation,
         const Settings &settings)
{
    if (observation.uptime_s < 0 || observation.taken == util::TimePoint{})
        return false;
    util::TimePoint boot = boot_time (observation);

    bool same_boot = false;
    if (host.last && !host.boots.empty ()
            && host.last->taken_steady != steady_clock::time_point{}
            && observation.taken_steady != steady_clock::time_point{})
        {
            double elapsed = duration<double> (observation.taken_steady
                                               - host.last->taken_steady).count ();
            same_boot = observation.uptime_s + same_boot_slack_s
                        >= host.last->uptime_s + elapsed;
        }
    host.last = observation;

    if (same_boot)
        {
            if (gap (host.boots.back (), boot) <= settings.same_boot)
                return false;
            host.boots.back () = boot;
            normalize (host, settings);
            return true;
        }

    for (util::TimePoint known : host.boots)
        if (gap (known, boot) <= settings.same_boot)
            return false;
    host.boots.push_back (boot);
    normalize (host, settings);
    return true;
}

std::vector<Boot>
all_boots (const History &history)
{
    std::vector<Boot> boots;
    for (const auto &[name, host] : history)
        for (util::TimePoint at : host.boots)
            boots.push_back ({name, at, is_router (host.kind), host.battery});
    std::ranges::sort (boots, {}, &Boot::at);
    return boots;
}

std::vector<PowerEvent>
power_events (const History &history, const Settings &settings)
{
    std::vector<Boot> boots = all_boots (history);
    std::vector<PowerEvent> events;
    std::size_t index = 0;
    while (index < boots.size ())
        {
            PowerEvent event;
            event.at = boots[index].at;
            std::set<std::string> seen;
            int without_battery = 0;
            std::size_t next = index;
            for (; next < boots.size ()
                    && boots[next].at - event.at <= settings.window; ++next)
                {
                    const Boot &boot = boots[next];
                    if (!seen.insert (boot.host).second)
                        continue;
                    event.boots.push_back (boot);
                    event.router = event.router || boot.router;
                    if (!boot.battery)
                        ++without_battery;
                }
            index = next;
            if (event.router || (seen.size () >= 2 && without_battery >= 1))
                events.push_back (std::move (event));
        }
    std::ranges::reverse (events);
    return events;
}

std::string
event_hosts (const PowerEvent &event)
{
    std::string names;
    for (const Boot &boot : event.boots)
        names += (names.empty () ? "" : ", ") + boot.host;
    return names;
}

std::vector<Cell>
timeline (const Lane &lane, util::TimePoint from, util::TimePoint to,
          int columns)
{
    std::vector<Cell> cells (static_cast<std::size_t> (std::max (columns, 0)),
                             Cell::unknown);
    if (columns <= 0 || to <= from || lane.boots.empty ())
        return cells;
    auto span = to - from;
    for (int column = 0; column < columns; ++column)
        {
            util::TimePoint start = from + span * column / columns;
            util::TimePoint end = from + span * (column + 1) / columns;
            Cell cell = Cell::up;
            auto boot = std::ranges::find_if (lane.boots, [&] (util::TimePoint at)
            {
                return at >= start && at < end;
            });
            if (boot != lane.boots.end ())
                cell = std::ranges::find (lane.cuts, *boot) != lane.cuts.end ()
                       ? Cell::cut : Cell::boot;
            else if (end <= lane.boots.front ())
                cell = Cell::unknown;
            else if (!lane.up_now && start >= lane.last_seen)
                cell = lane.last_seen == util::TimePoint{} ? Cell::unknown
                       : Cell::down;
            cells[static_cast<std::size_t> (column)] = cell;
        }
    return cells;
}

std::string
format_duration (seconds duration)
{
    long total = std::max<long> (duration.count (), 0);
    long day_count = total / 86400;
    long hour_count = total % 86400 / 3600;
    long minute_count = total % 3600 / 60;
    if (day_count > 0)
        return std::format ("{}d {}h", day_count, hour_count);
    if (hour_count > 0)
        return std::format ("{}h {}m", hour_count, minute_count);
    if (minute_count > 0)
        return std::format ("{}m", minute_count);
    return std::format ("{}s", total);
}

std::string
format_when (util::TimePoint when, const time_zone *zone)
{
    return std::format ("{:%a %d %b %H:%M}", util::to_local (when, zone));
}

std::string
history_to_json (const History &history)
{
    json hosts = json::object ();
    for (const auto &[name, host] : history)
        {
            json boots = json::array ();
            for (util::TimePoint boot : host.boots)
                boots.push_back (duration_cast<seconds> (boot.time_since_epoch ())
                                 .count ());
            hosts[name] = {{"kind", host.kind}, {"battery", host.battery},
                {"boots", std::move (boots)}
            };
        }
    return json{{"version", 1}, {"hosts", std::move (hosts)}}.dump (1);
}

History
history_from_json (std::string_view text)
{
    History history;
    json parsed = json::parse (text, nullptr, false);
    if (!parsed.is_object ())
        return history;
    auto hosts = parsed.find ("hosts");
    if (hosts == parsed.end () || !hosts->is_object ())
        return history;
    for (const auto &[name, entry] : hosts->items ())
        {
            if (!entry.is_object ())
                continue;
            HostHistory host;
            host.kind = entry.value ("kind", std::string ());
            host.battery = entry.value ("battery", false);
            if (auto boots = entry.find ("boots");
                    boots != entry.end () && boots->is_array ())
                for (const json &boot : *boots)
                    if (boot.is_number_integer ())
                        host.boots.push_back (util::TimePoint{seconds{boot.get<long long> ()}});
            std::ranges::sort (host.boots);
            history[name] = std::move (host);
        }
    return history;
}

namespace
{
bool
apply (History &history, const std::vector<machines::Host> &hosts,
       const Settings &settings)
{
    bool changed = false;
    for (const machines::Host &host : hosts)
        {
            machines::Reading reading = host.read ();
            if (!reading.stats || !reading.stats->uptime_s
                    || reading.taken == util::TimePoint{})
                continue;
            HostHistory &entry = history[host.name];
            std::string kind (machines::kind_name (host.kind));
            bool battery = reading.stats->battery_pct.has_value ();
            if (entry.kind != kind || entry.battery != battery)
                {
                    entry.kind = std::move (kind);
                    entry.battery = battery;
                    changed = true;
                }
            if (observe (entry, {reading.taken, reading.taken_steady,
                                 *reading.stats->uptime_s
                                }, settings))
                changed = true;
        }
    return changed;
}

class BootSource : public Source
{
public:
    BootSource (const std::vector<machines::Host> &hosts, Settings settings,
                seconds interval)
        : Source ("uptime", interval, interval), hosts_ (hosts),
          settings_ (settings)
    {
    }

    ~BootSource () override
    {
        stop ();
    }

    Latest<History> history;

    History
    current () const
    {
        auto stored = history.get ();
        History copy = stored ? *stored : History{};
        apply (copy, hosts_, settings_);
        return copy;
    }

protected:
    std::optional<util::TimePoint>
    restore () override
    {
        load ();
        return std::nullopt;
    }

    void
    fetch () override
    {
        if (!history.get ())
            load ();
        auto stored = history.get ();
        History updated = stored ? *stored : History{};
        bool changed = apply (updated, hosts_, settings_);
        if (changed)
            cache_put ("history", history_to_json (updated));
        history.set (std::move (updated));
    }

private:
    void
    load ()
    {
        History loaded;
        if (auto cached = cache_get ("history"))
            loaded = history_from_json (cached->body);
        for (auto &[name, host] : loaded)
            normalize (host, settings_);
        history.set (std::move (loaded));
    }

    const std::vector<machines::Host> &hosts_;
    Settings settings_;
};

const char *
cell_glyph (Cell cell)
{
    switch (cell)
        {
        case Cell::up:
            return "━";
        case Cell::down:
            return "╌";
        case Cell::boot:
        case Cell::cut:
            return "┃";
        case Cell::unknown:
            break;
        }
    return " ";
}

tui::Role
cell_role (Cell cell)
{
    switch (cell)
        {
        case Cell::up:
            return tui::GOOD;
        case Cell::down:
            return tui::BAD;
        case Cell::boot:
            return tui::WARN;
        case Cell::cut:
            return tui::BAD;
        case Cell::unknown:
            break;
        }
    return tui::DIM;
}

class UptimeScreen : public Screen
{
public:
    UptimeScreen (BootSource &boot, const std::vector<machines::Host> &hosts,
                  Settings settings, int days, const time_zone *zone)
        : boot_ (boot), hosts_ (hosts), settings_ (settings), days_ (days),
          zone_ (zone)
    {
    }

    std::string title () const override
    {
        return "Uptime";
    }

    std::vector<Source *>
    sources () const override
    {
        std::vector<Source *> out{&boot_};
        for (const machines::Host &host : hosts_)
            if (host.source)
                out.push_back (host.source);
        return out;
    }

    std::vector<Source *>
    footer_sources () const override
    {
        std::vector<Source *> out{&boot_};
        for (const machines::Host &host : hosts_)
            if (host.source && host.source->status ().error.empty ())
                out.push_back (host.source);
        return out;
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        History history = boot_.current ();
        std::vector<PowerEvent> events = power_events (history, settings_);

        int host_count = static_cast<int> (hosts_.size ());
        spacing_ = area.h >= 2 * host_count + 5 + 16 ? 2 : 1;
        int table_height = std::min (host_count * spacing_ + 4 + spacing_ - 1,
                                     std::max (area.h - 7, 5));
        tui::Rect table = area.top (table_height);
        tui::Rect power = area.below (table_height);
        draw_table (table, history, events, now);
        draw_power (power, history, events, now);
    }

private:
    struct Row
    {
        const machines::Host *host;
        machines::Reading reading;
        bool up = false;
        const HostHistory *history = nullptr;
    };

    std::vector<Row>
    rows (const History &history) const
    {
        std::vector<Row> out;
        for (const machines::Host &host : hosts_)
            {
                Row row{&host, host.read (), false, nullptr};
                row.up = host.source && host.source->status ().error.empty ()
                         && row.reading.stats && row.reading.taken != util::TimePoint{};
                if (auto found = history.find (host.name); found != history.end ())
                    row.history = &found->second;
                out.push_back (std::move (row));
            }
        return out;
    }

    void
    draw_table (const tui::Rect &area, const History &history,
                const std::vector<PowerEvent> &events, util::TimePoint now)
    {
        std::vector<Row> table_rows = rows (history);
        int up_count = static_cast<int> (std::ranges::count (table_rows, true,
                                         &Row::up));
        tui::Rect inside = tui::frame (area, std::format (
                                           "{} Uptime · {} of {} up", tui::icon ("\U000F0150", "◷"),
                                           up_count, table_rows.size ()));
        if (inside.empty ())
            return;
        if (table_rows.empty ())
            {
                tui::put (inside.y, inside.x + 1, "No hosts in [machines].",
                          tui::attr (tui::DIM));
                return;
            }

        int name_width = 6;
        for (const Row &row : table_rows)
            name_width = std::max (name_width, tui::width (row.host->name) + 2);
        name_width = std::min (name_width, 18);
        constexpr int status_width = 2;
        constexpr int kind_width = 10;
        constexpr int address_width = 16;
        constexpr int uptime_width = 14;
        constexpr int booted_width = 25;
        constexpr int count_width = 7;
        int fixed = 1 + status_width + name_width + uptime_width + booted_width
                    + count_width + 1;
        int timeline_width = inside.w - fixed - kind_width - address_width;
        bool show_kind = timeline_width >= 6 * days_;
        if (!show_kind)
            timeline_width += kind_width;
        bool show_address = timeline_width >= 3 * days_;
        if (!show_address)
            timeline_width += address_width;
        bool show_timeline = timeline_width >= 2 * days_;

        int x_status = inside.x + 1;
        int x_name = x_status + status_width;
        int x_kind = x_name + name_width;
        int x_address = x_kind + (show_kind ? kind_width : 0);
        int x_uptime = x_address + (show_address ? address_width : 0);
        int x_booted = x_uptime + uptime_width;
        int x_count = x_booted + booted_width;
        int x_timeline = x_count + count_width;

        util::TimePoint from = now - days{days_};
        int row_y = inside.y;
        attr_t header_attr = tui::attr (tui::DIM) | A_BOLD;
        tui::put (row_y, x_name, "host", header_attr);
        if (show_kind)
            tui::put (row_y, x_kind, "kind", header_attr);
        if (show_address)
            tui::put (row_y, x_address, "address", header_attr);
        tui::put (row_y, x_uptime, "uptime", header_attr);
        tui::put (row_y, x_booted, "last boot", header_attr);
        tui::put (row_y, x_count, "boots", header_attr);
        if (show_timeline)
            draw_day_labels (row_y, x_timeline, timeline_width, from, now);
        row_y += spacing_;

        for (const Row &row : table_rows)
            {
                if (row_y >= inside.bottom () - 1)
                    break;
                draw_row (row_y, row, events, from, now, show_kind, show_address,
                          show_timeline,
                {x_status, x_name, x_kind, x_address, x_uptime, x_booted, x_count, x_timeline},
                name_width, timeline_width);
                row_y += spacing_;
            }
        if (spacing_ > 1)
            row_y = std::min (row_y, inside.bottom () - 1);

        if (show_timeline && row_y < inside.bottom ())
            {
                constexpr int legend_width = 47;
                int legend_x = std::max (x_name, inside.right () - 1 - legend_width);
                auto legend = [&] (Cell cell, const char *text)
                {
                    int room = inside.right () - legend_x;
                    if (room < 2)
                        return;
                    legend_x += tui::put (row_y, legend_x, cell_glyph (cell),
                                          tui::attr (cell_role (cell)));
                    legend_x += tui::put (row_y, legend_x, std::format (" {}   ", text),
                                          tui::attr (tui::DIM), room - 1);
                };
                legend (Cell::up, "up");
                legend (Cell::boot, "boot");
                legend (Cell::cut, "power cut");
                legend (Cell::down, "not answering");
            }
    }

    struct Columns
    {
        int status, name, kind, address, uptime, booted, count, timeline;
    };

    void
    draw_day_labels (int row_y, int left, int width, util::TimePoint from,
                     util::TimePoint to) const
    {
        auto span = to - from;
        bool long_labels = width / days_ >= 8;
        local_days first_day = floor<days> (util::to_local (from, zone_)) + days{1};
        int free_x = left;
        for (local_days day = first_day; ; day += days{1})
            {
                util::TimePoint midnight;
                try
                    {
                        midnight = zone_->to_sys (local_seconds{day});
                    }
                catch (const std::exception &)
                    {
                        continue;
                    }
                if (midnight >= to)
                    break;
                int column = static_cast<int> ((midnight - from) * width / span);
                std::string label = long_labels
                                    ? std::format ("╷{:%a %d}", day)
                                    : std::format ("╷{:%a}", day);
                if (left + column < free_x || column + tui::width (label) > width)
                    continue;
                free_x = left + column + tui::put (row_y, left + column, label,
                                                   tui::attr (tui::DIM)) + 1;
            }
    }

    void
    draw_row (int row_y, const Row &row, const std::vector<PowerEvent> &events,
              util::TimePoint from, util::TimePoint now, bool show_kind,
              bool show_address, bool show_timeline, Columns columns,
              int name_width, int timeline_width) const
    {
        const machines::Host &host = *row.host;
        bool seen = row.reading.stats && row.reading.taken != util::TimePoint{};
        bool waiting = false;
        if (host.source && !row.up)
            {
                Source::Status status = host.source->status ();
                waiting = status.error.empty () && !status.has_data ();
            }
        tui::Role status_role = !host.source || waiting ? tui::DIM
                                : row.up ? tui::GOOD : tui::BAD;
        tui::put (row_y, columns.status, host.source ? "●" : "○",
                  tui::attr (status_role));
        tui::put (row_y, columns.name, tui::fit (host.name, name_width - 1),
                  tui::attr (tui::TITLE) | A_BOLD);
        if (show_kind)
            tui::put (row_y, columns.kind, std::string (machines::kind_name (host.kind)),
                      tui::attr (tui::DIM));
        if (show_address)
            tui::put (row_y, columns.address,
                      !host.address.empty () ? tui::fit (host.address, 15)
                      : host.kind == machines::Kind::local ? std::string ("this machine")
                      : std::string ("—"),
                      tui::attr (host.address.empty () ? tui::DIM : tui::NORMAL));

        std::string uptime_text;
        tui::Role uptime_role = tui::DIM;
        if (!host.source)
            uptime_text = "not polled";
        else if (row.up && row.reading.stats->uptime_s)
            {
                auto uptime = seconds{static_cast<long> (*row.reading.stats->uptime_s)}
                              + duration_cast<seconds> (now - row.reading.taken);
                uptime_text = "up " + format_duration (uptime);
                uptime_role = tui::GOOD;
            }
        else if (row.up)
            uptime_text = "up";
        else if (waiting)
            uptime_text = "asking…";
        else if (seen)
            {
                uptime_text = "down " + format_duration (
                                  duration_cast<seconds> (now - row.reading.taken));
                uptime_role = tui::BAD;
            }
        else
            {
                uptime_text = "no answer";
                uptime_role = tui::BAD;
            }
        tui::put (row_y, columns.uptime, uptime_text, tui::attr (uptime_role));

        int recent = 0;
        Lane lane;
        if (row.history && !row.history->boots.empty ())
            {
                util::TimePoint last_boot = row.history->boots.back ();
                tui::put (row_y, columns.booted, "booted " + format_when (last_boot, zone_),
                          tui::attr (row.up ? tui::NORMAL : tui::DIM));
                lane.boots = row.history->boots;
                recent = static_cast<int> (std::ranges::count_if (lane.boots,
                                           [from] (util::TimePoint at)
                {
                    return at >= from;
                }));
            }
        else
            tui::put (row_y, columns.booted, "—", tui::attr (tui::DIM));
        tui::put (row_y, columns.count, recent ? std::to_string (recent) : "·",
                  tui::attr (recent > 1 ? tui::WARN : tui::DIM));

        if (!show_timeline)
            return;
        for (const PowerEvent &event : events)
            for (const Boot &boot : event.boots)
                if (boot.host == host.name)
                    lane.cuts.push_back (boot.at);
        lane.last_seen = seen ? row.reading.taken : util::TimePoint{};
        lane.up_now = row.up;
        std::vector<Cell> cells = timeline (lane, from, now, timeline_width);
        for (int column = 0; column < timeline_width; ++column)
            {
                Cell cell = cells[static_cast<std::size_t> (column)];
                attr_t cell_attr = tui::attr (cell_role (cell));
                if (cell == Cell::cut)
                    cell_attr |= A_BOLD;
                tui::put (row_y, columns.timeline + column,
                          cell == Cell::unknown ? "·" : cell_glyph (cell), cell_attr);
            }
    }

    void
    draw_power (const tui::Rect &area, const History &history,
                const std::vector<PowerEvent> &events, util::TimePoint now) const
    {
        tui::Rect inside = tui::frame (area, std::format ("{} Power",
                                       tui::icon ("\U000F06A5", "⚡")));
        if (inside.empty ())
            return;
        int row_y = inside.y;
        int left = inside.x + 1;
        int width = inside.w - 2;
        if (events.empty ())
            {
                tui::put (row_y, left, tui::fit ("No power cut in the recorded boot history.",
                                                 width), tui::attr (tui::GOOD));
            }
        else
            {
                const PowerEvent &last = events.front ();
                int used = tui::put (row_y, left, "Last power cut: ",
                                     tui::attr (tui::DIM));
                used += tui::put (row_y, left + used, format_when (last.at, zone_),
                                  tui::attr (tui::BAD) | A_BOLD);
                used += tui::put (row_y, left + used, tui::fit (std::format (
                                      " ({} restarted), {} ago", event_hosts (last),
                                      format_duration (duration_cast<seconds> (now - last.at))),
                                  std::max (0, width - used)), tui::attr (tui::TITLE));
                std::string stayed;
                for (const machines::Host &host : hosts_)
                    {
                        auto found = history.find (host.name);
                        if (found == history.end () || found->second.boots.empty ()
                                || !host.source || !host.source->status ().error.empty ())
                            continue;
                        if (found->second.boots.back () < last.at)
                            stayed += (stayed.empty () ? "" : ", ") + host.name;
                    }
                if (!stayed.empty () && row_y + 1 < inside.bottom ())
                    {
                        tui::put (row_y + 1, left, tui::fit ("Kept running through it: " + stayed,
                                                             width), tui::attr (tui::DIM));
                        ++row_y;
                    }
            }
        row_y += 2;
        if (row_y >= inside.bottom ())
            return;

        tui::Rect lists{row_y, left, inside.bottom () - row_y, width};
        bool two_columns = width >= 90;
        std::vector<tui::Rect> halves = two_columns ? lists.columns (2, 4)
                                        : std::vector<tui::Rect> {lists};
        draw_event_list (halves[0], events, now);
        if (two_columns)
            draw_boot_list (halves[1], history, events, now);
    }

    void
    draw_event_list (const tui::Rect &area, const std::vector<PowerEvent> &events,
                     util::TimePoint now) const
    {
        attr_t header_attr = tui::attr (tui::DIM) | A_BOLD;
        tui::put (area.y, area.x, std::format ("Power cuts ({})", events.size ()),
                  header_attr);
        int row_y = area.y + 1;
        for (const PowerEvent &event : events)
            {
                if (row_y >= area.bottom ())
                    break;
                int used = tui::put (row_y, area.x, format_when (event.at, zone_),
                                     tui::attr (tui::BAD));
                used += tui::put (row_y, area.x + used, tui::pad (std::format (
                                      "  {} ago", util::format_age (event.at, now)), 11),
                                  tui::attr (tui::DIM));
                tui::put (row_y, area.x + used, tui::fit (event_hosts (event),
                          area.w - used), tui::attr (tui::NORMAL));
                ++row_y;
            }
        if (events.empty () && row_y < area.bottom ())
            tui::put (row_y, area.x, "none detected", tui::attr (tui::DIM));
    }

    void
    draw_boot_list (const tui::Rect &area, const History &history,
                    const std::vector<PowerEvent> &events, util::TimePoint now) const
    {
        attr_t header_attr = tui::attr (tui::DIM) | A_BOLD;
        tui::put (area.y, area.x, "Recent boots", header_attr);
        std::vector<Boot> boots = all_boots (history);
        std::ranges::reverse (boots);
        int row_y = area.y + 1;
        for (const Boot &boot : boots)
            {
                if (row_y >= area.bottom ())
                    break;
                bool in_cut = std::ranges::any_of (events, [&boot] (const PowerEvent &event)
                {
                    return std::ranges::any_of (event.boots, [&boot] (const Boot &member)
                    {
                        return member.host == boot.host && member.at == boot.at;
                    });
                });
                int used = tui::put (row_y, area.x, format_when (boot.at, zone_),
                                     tui::attr (in_cut ? tui::BAD : tui::WARN));
                used += tui::put (row_y, area.x + used, tui::pad (std::format (
                                      "  {} ago", util::format_age (boot.at, now)), 11),
                                  tui::attr (tui::DIM));
                used += tui::put (row_y, area.x + used, tui::fit (boot.host, 16),
                                  tui::attr (tui::TITLE));
                tui::put (row_y, area.x + used, in_cut ? "  power cut" : "",
                          tui::attr (tui::DIM));
                ++row_y;
            }
        if (boots.empty () && row_y < area.bottom ())
            tui::put (row_y, area.x, "waiting for the hosts to answer…",
                      tui::attr (tui::DIM));
    }

    BootSource &boot_;
    const std::vector<machines::Host> &hosts_;
    Settings settings_;
    int days_;
    const time_zone *zone_;
    int spacing_ = 1;
};
}
}

void
install_uptime (App &app)
{
    using namespace uptime;
    const toml::table &table = app.config ().section ("uptime");
    Settings settings;
    settings.keep = static_cast<std::size_t> (std::clamp (
                        table["keep"].value_or (50), 2, 1000));
    settings.same_boot = minutes{std::clamp (table["same_boot_minutes"].value_or (3),
                                 1, 60)};
    settings.window = minutes{std::clamp (table["window_minutes"].value_or (10),
                                          1, 240)};
    int days_shown = std::clamp (table["days"].value_or (7), 1, 60);
    const time_zone *zone = util::zone (table["timezone"].value_or<std::string> (""));
    seconds interval{std::max<long> (table["interval"].value_or (60), 5)};

    const std::vector<machines::Host> &hosts = machines::hosts (app);
    auto &boot = app.add_source (std::make_unique<BootSource> (hosts, settings,
                                 interval));
    app.add_screen (std::make_unique<UptimeScreen> (boot, hosts, settings,
                    days_shown, zone));
}
