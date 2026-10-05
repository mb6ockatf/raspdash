/* source.hpp -- background data sources.

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
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "util.hpp"

template <typename T>
class Latest
{
public:
    void
    set (T value)
    {
        auto p = std::make_shared<const T> (std::move (value));
        std::lock_guard lock (m_);
        v_ = std::move (p);
    }

    std::shared_ptr<const T>
    get () const
    {
        std::lock_guard lock (m_);
        return v_;
    }

private:
    mutable std::mutex m_;
    std::shared_ptr<const T> v_;
};

class Source
{
public:
    struct Status
    {
        util::TimePoint updated{};
        util::TimePoint error_at{};
        std::string error;
        bool busy = false;

        bool has_data () const
        {
            return updated != util::TimePoint{};
        }
    };

    Source (std::string name, std::chrono::seconds interval,
            std::chrono::seconds retry = std::chrono::seconds{60});
    virtual ~Source ();
    Source (const Source &) = delete;
    Source &operator= (const Source &) = delete;

    const std::string &name () const
    {
        return name_;
    }
    Status status () const;

    void start ();
    void stop ();

    void refresh_now ();

    void poll_once ();

    static void set_cache_dir (std::filesystem::path dir);

    struct Cached
    {
        std::string body;
        util::TimePoint saved;
    };

protected:
    virtual void fetch () = 0;

    virtual std::optional<util::TimePoint> restore ()
    {
        return std::nullopt;
    }

    virtual bool expired (util::TimePoint now) const
    {
        (void) now;
        return false;
    }

    void cache_put (std::string_view key, std::string_view body) const;
    std::optional<Cached> cache_get (std::string_view key) const;

private:
    void run (std::stop_token st);
    std::filesystem::path cache_path (std::string_view key) const;

    std::string name_;
    std::chrono::seconds interval_;
    std::chrono::seconds retry_;
    mutable std::mutex m_;
    std::condition_variable_any cv_;
    bool poked_ = false;
    Status status_;
    std::jthread thread_;
};
