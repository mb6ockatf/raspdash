/* source.cpp -- background data sources.

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

#include "source.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{
fs::path cache_dir_;

constexpr std::chrono::seconds expiry_check = 5min;
constexpr std::chrono::seconds max_backoff = 30min;
}

void
Source::set_cache_dir (fs::path dir)
{
    cache_dir_ = std::move (dir);
}

fs::path
Source::cache_path (std::string_view key) const
{
    std::string file = name_ + "-" + std::string (key);
    for (char &c : file)
        if (!(std::isalnum (static_cast<unsigned char> (c)) || c == '-'
                || c == '_' || c == '.'))
            c = '_';
    return cache_dir_ / file;
}

void
Source::cache_put (std::string_view key, std::string_view body) const
{
    if (cache_dir_.empty ())
        return;
    std::error_code ec;
    fs::create_directories (cache_dir_, ec);
    fs::path final_path = cache_path (key);
    fs::path tmp = final_path;
    tmp += ".tmp" + std::to_string (::getpid ());
    {
        std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        out.write (body.data (), static_cast<std::streamsize> (body.size ()));
        if (!out)
            {
                fs::remove (tmp, ec);
                return;
            }
    }
    fs::rename (tmp, final_path, ec);
    if (ec)
        fs::remove (tmp, ec);
}

std::optional<Source::Cached>
Source::cache_get (std::string_view key) const
{
    if (cache_dir_.empty ())
        return std::nullopt;
    fs::path p = cache_path (key);
    std::error_code ec;
    auto mtime = fs::last_write_time (p, ec);
    if (ec)
        return std::nullopt;
    std::ifstream in (p, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf ();
    return Cached{ss.str (), std::chrono::clock_cast<util::Clock> (mtime)};
}

Source::Source (std::string name, std::chrono::seconds interval,
                std::chrono::seconds retry)
    : name_ (std::move (name)), interval_ (interval),
      retry_ (std::min (retry, interval))
{
}

Source::~Source ()
{
    stop ();
}

Source::Status
Source::status () const
{
    std::lock_guard lock (m_);
    return status_;
}

void
Source::start ()
{
    if (!thread_.joinable ())
        thread_ = std::jthread ([this] (std::stop_token st)
        {
            run (st);
        });
}

void
Source::stop ()
{
    if (thread_.joinable ())
        {
            thread_.request_stop ();
            cv_.notify_all ();
            thread_.join ();
        }
}

void
Source::refresh_now ()
{
    {
        std::lock_guard lock (m_);
        poked_ = true;
    }
    cv_.notify_all ();
}

void
Source::poll_once ()
{
    {
        std::lock_guard lock (m_);
        status_.busy = true;
    }
    std::string err;
    try
        {
            fetch ();
        }
    catch (const std::exception &e)
        {
            err = e.what ();
            if (err.empty ())
                err = "unknown error";
        }
    catch (...)
        {
            err = "unknown error";
        }
    std::lock_guard lock (m_);
    status_.busy = false;
    if (err.empty ())
        {
            status_.updated = util::Clock::now ();
            status_.error.clear ();
        }
    else
        {
            status_.error_at = util::Clock::now ();
            status_.error = std::move (err);
        }
}

void
Source::run (std::stop_token st)
{
    std::optional<util::TimePoint> restored;
    try
        {
            restored = restore ();
        }
    catch (...)
        {
            restored.reset ();
        }
    if (restored)
        {
            std::lock_guard lock (m_);
            status_.updated = *restored;
        }

    std::chrono::seconds backoff = retry_;
    while (!st.stop_requested ())
        {
            auto now = util::Clock::now ();
            Status s = status ();
            bool due;
            {
                std::lock_guard lock (m_);
                due = poked_;
                poked_ = false;
            }
            if (!s.error.empty () && s.error_at > s.updated)
                due = due || now - s.error_at >= backoff;
            else
                due = due || !s.has_data () || now - s.updated >= interval_;
            if (!due)
                try
                    {
                        due = expired (now);
                    }
                catch (...)
                    {
                        due = true;
                    }

            if (due)
                {
                    poll_once ();
                    Status after = status ();
                    if (after.error.empty ())
                        backoff = retry_;
                    else if (s.error.empty () || s.error_at <= s.updated)
                        backoff = retry_;
                    else
                        backoff = std::min ({backoff * 2, interval_,
                                             max_backoff});
                    continue;
                }

            std::chrono::seconds wait = expiry_check;
            if (!s.error.empty () && s.error_at > s.updated)
                wait = std::min (wait, std::chrono::ceil<std::chrono::seconds>
                                 (s.error_at + backoff - now));
            else if (s.has_data ())
                wait = std::min (wait, std::chrono::ceil<std::chrono::seconds>
                                 (s.updated + interval_ - now));
            wait = std::max (wait, std::chrono::seconds{1});
            std::unique_lock lock (m_);
            cv_.wait_for (lock, st, wait, [this]
            {
                return poked_;
            });
        }
}
