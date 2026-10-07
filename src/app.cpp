/* app.cpp -- the dashboard application: screens, sources, main loop.

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

#include "app.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <format>
#include <print>
#include <stdexcept>
#include <thread>

#include "tui.hpp"

namespace
{
volatile std::sig_atomic_t quit_signal = 0;

extern "C" void
on_signal (int)
{
    quit_signal = 1;
}
}

App::App (const Config &cfg) : cfg_ (cfg)
{
    const toml::table &app = cfg.section ("app");
    rotate_ = std::chrono::seconds (app["rotate_seconds"].value_or (60));
    pause_after_key_
        = std::chrono::seconds (app["pause_after_key_seconds"].value_or (120));
    allow_quit_ = app["allow_quit"].value_or (true);
    clock_format_ = app["clock_format"].value_or<std::string> ("%a %d %b  %H:%M");
    show_ip_ = app["show_ip"].value_or (true);
    std::string icons = app["icons"].value_or<std::string> ("nerd");
    if (icons != "nerd" && icons != "unicode")
        throw std::runtime_error ("[app] icons must be \"nerd\" or \"unicode\"");
    tui::set_nerd_icons (icons == "nerd");

    std::string cache = app["cache_dir"].value_or<std::string> ("");
    if (cache.empty ())
        {
            const char *xdg = std::getenv ("XDG_CACHE_HOME");
            const char *home = std::getenv ("HOME");
            if (xdg && *xdg)
                cache = std::string (xdg) + "/raspdash";
            else if (home && *home)
                cache = std::string (home) + "/.cache/raspdash";
        }
    else
        cache = cfg.resolve (cache).string ();
    Source::set_cache_dir (cache);
}

App::~App ()
{
    stop_sources ();
}

void
App::stop_sources ()
{

    for (auto &s : sources_)
        s->stop ();
}

void
App::add_screen (std::unique_ptr<Screen> s)
{
    screens_.push_back (std::move (s));
}

void
App::select (std::size_t i)
{
    if (screens_.empty ())
        return;
    current_ = i % screens_.size ();
    switched_at_ = std::chrono::steady_clock::now ();
    clear_pending_ = true;
}

void
App::draw_header (util::TimePoint now)
{
    int x = 0;
    for (std::size_t i = 0; i < screens_.size (); ++i)
        {
            std::string tab = std::format (" {} {} ", i + 1, screens_[i]->title ());
            x += tui::put (0, x, tab,
                           tui::attr (i == current_ ? tui::TAB_ON : tui::TAB_OFF));
            x += tui::put (0, x, " ");
        }
    std::string clock;
    try
        {
            auto local = util::to_local (now, util::zone ());
            clock = std::vformat (std::string ("{:") + clock_format_ + "}",
                                  std::make_format_args (local));
        }
    catch (const std::exception &)
        {
            clock = "??:??";
        }
    int cw = tui::width (clock);
    if (COLS - cw - 1 > x)
        tui::put (0, COLS - cw - 1, clock, tui::attr (tui::TITLE));
    else
        return;
    if (!show_ip_)
        return;
    std::optional<util::Interface> primary = util::primary_ipv4 ();
    std::string address = std::format ("{} {}", tui::icon ("\U000F0317", "⇄"),
                                       primary ? primary->address : "no network");
    int address_width = tui::width (address);
    int address_x = COLS - cw - 1 - 3 - address_width;
    if (address_x > x + 1)
        tui::put (0, address_x, address,
                  tui::attr (primary ? tui::DIM : tui::BAD));
}

void
App::draw_footer (util::TimePoint now)
{
    int y = LINES - 1;
    std::string help = allow_quit_ ? "←/→ 0-9 screens  r refresh  q quit"
                       : "←/→ 0-9 screens  r refresh";
    int hw = tui::width (help);
    int limit = COLS - 1;
    if (COLS - hw - 1 > 40)
        {
            tui::put (y, COLS - hw - 1, help, tui::attr (tui::DIM));
            limit = COLS - hw - 3;
        }

    std::vector<std::pair<std::string, tui::Role>> items;
    std::vector<Source::Status> healthy;
    std::vector<Source *> healthy_sources;
    for (Source *s : screens_[current_]->footer_sources ())
        {
            Source::Status st = s->status ();
            if (!st.error.empty ())
                {
                    std::string text = std::format ("{} ✗ {}: {}", s->name (),
                                                    util::format_age (st.error_at, now),
                                                    st.error);
                    if (st.has_data ())
                        text += std::format (" (data {} old)",
                                             util::format_age (st.updated, now));
                    items.emplace_back (std::move (text), tui::BAD);
                }
            else
                {
                    healthy.push_back (st);
                    healthy_sources.push_back (s);
                }
        }

    if (healthy.size () <= 2)
        for (std::size_t i = 0; i < healthy.size (); ++i)
            {
                const Source::Status &st = healthy[i];
                std::string text = st.has_data ()
                                   ? std::format ("{} ✓ {} ago", healthy_sources[i]->name (),
                                                  util::format_age (st.updated, now))
                                   : std::format ("{} … loading", healthy_sources[i]->name ());
                if (st.busy)
                    text += " ⟳";
                items.emplace_back (std::move (text), tui::DIM);
            }
    else
        {
            std::size_t loading = 0;
            util::TimePoint oldest = util::Clock::now ();
            for (const Source::Status &st : healthy)
                if (!st.has_data ())
                    ++loading;
                else
                    oldest = std::min (oldest, st.updated);
            std::string text = std::format ("{} ✓", healthy.size () - loading);
            if (loading < healthy.size ())
                text += std::format (" (oldest {} ago)", util::format_age (oldest, now));
            if (loading)
                text += std::format ("  {} … loading", loading);
            items.emplace_back (std::move (text), tui::DIM);
        }

    int failing = static_cast<int> (std::ranges::count (items, tui::BAD,
                                    &std::pair<std::string, tui::Role>::second));
    int cap = failing > 1 ? std::max (30, limit / failing - 3) : limit;
    int x = 0;
    for (const auto &[text, role] : items)
        {
            int room = std::min (limit - x, role == tui::BAD ? cap : limit);
            if (room <= 3)
                break;
            x += tui::put (y, x, tui::fit (text, room), tui::attr (role)) + 3;
        }
}

int
App::run (std::string_view start_screen)
{
    if (screens_.empty ())
        {
            std::println (stderr, "raspdash: no screens configured");
            return 1;
        }
    for (std::size_t i = 0; i < screens_.size (); ++i)
        if (screens_[i]->title () == start_screen)
            current_ = i;

    std::signal (SIGINT, on_signal);
    std::signal (SIGTERM, on_signal);
    std::signal (SIGHUP, on_signal);

    for (auto &s : sources_)
        s->start ();

    tui::init ();
    timeout (250);
    switched_at_ = std::chrono::steady_clock::now ();

    key_at_ = switched_at_ - pause_after_key_;

    while (!quit_signal)
        {
            auto now = util::Clock::now ();
            if (clear_pending_)
                {
                    clear ();
                    clear_pending_ = false;
                }
            else
                erase ();
            if (LINES >= 3 && COLS >= 10)
                {
                    draw_header (now);
                    tui::Rect body{1, 0, LINES - 2, COLS};
                    try
                        {
                            screens_[current_]->draw (body, now);
                        }
                    catch (const std::exception &e)
                        {
                            tui::put (body.y + 1, 1, std::format ("draw error: {}", e.what ()),
                                      tui::attr (tui::BAD));
                        }
                    draw_footer (now);
                }
            refresh ();

            int ch = getch ();
            auto steady = std::chrono::steady_clock::now ();
            if (ch != ERR && ch != KEY_RESIZE)
                {
                    key_at_ = steady;
                    if ((ch == 'q' || ch == 'Q') && allow_quit_)
                        break;
                    else if (ch >= '1' && ch <= '9'
                             && static_cast<std::size_t> (ch - '1') < screens_.size ())
                        select (static_cast<std::size_t> (ch - '1'));
                    else if (ch == '0' && screens_.size () >= 10)
                        select (9);
                    else if (ch == KEY_RIGHT || ch == '\t' || ch == ' ' || ch == 'l')
                        select (current_ + 1);
                    else if (ch == KEY_MOUSE)
                        {
                            MEVENT event;
                            if (getmouse (&event) == OK && (event.bstate & BUTTON1_PRESSED))
                                select (current_ + 1);
                        }
                    else if (ch == KEY_LEFT || ch == KEY_BTAB || ch == 'h')
                        select (current_ + screens_.size () - 1);
                    else if (ch == 'r' || ch == 'R')
                        for (Source *s : screens_[current_]->sources ())
                            s->refresh_now ();
                    else
                        screens_[current_]->key (ch);
                }
            if (rotate_.count () > 0 && screens_.size () > 1
                    && steady - switched_at_ >= rotate_
                    && steady - key_at_ >= pause_after_key_)
                select (current_ + 1);
        }

    tui::shutdown ();
    stop_sources ();
    return 0;
}

int
App::check ()
{

    {
        std::vector<std::jthread> workers;
        for (auto &s : sources_)
            workers.emplace_back ([&s]
            {
                s->poll_once ();
            });
    }
    int failed = 0;
    for (auto &s : sources_)
        {
            Source::Status st = s->status ();
            if (st.error.empty ())
                std::println ("ok    {}", s->name ());
            else
                {
                    std::println ("FAIL  {}: {}", s->name (), st.error);
                    ++failed;
                }
        }
    return failed ? 1 : 0;
}
