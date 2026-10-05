/* app.hpp -- the dashboard application: screens, sources, main loop.

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
#include <memory>
#include <string>
#include <vector>

#include "config.hpp"
#include "screen.hpp"
#include "source.hpp"

class App
{
public:
    explicit App (const Config &cfg);
    ~App ();

    const Config &config () const
    {
        return cfg_;
    }

    template <typename T>
    T &
    add_source (std::unique_ptr<T> s)
    {
        T &ref = *s;
        sources_.push_back (std::move (s));
        return ref;
    }

    void add_screen (std::unique_ptr<Screen> s);

    int run (std::string_view start_screen = {});

    int check ();

    void stop_sources ();

private:
    void draw_header (util::TimePoint now);
    void draw_footer (util::TimePoint now);
    void select (std::size_t i);

    const Config &cfg_;
    std::vector<std::unique_ptr<Source>> sources_;
    std::vector<std::unique_ptr<Screen>> screens_;
    std::size_t current_ = 0;
    bool clear_pending_ = false;

    std::chrono::seconds rotate_{0};
    std::chrono::seconds pause_after_key_{120};
    bool allow_quit_ = true;
    std::string clock_format_;
    std::chrono::steady_clock::time_point switched_at_;
    std::chrono::steady_clock::time_point key_at_;
};
