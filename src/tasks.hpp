/* tasks.hpp -- read-only Google Tasks.

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
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace tasks
{
inline constexpr std::string_view scope
    = "https://www.googleapis.com/auth/tasks.readonly";

struct Task
{
    std::string id;
    std::string title;
    std::string notes;
    std::string parent;
    std::string position;
    bool completed = false;

    std::optional<std::chrono::year_month_day> due;
    int depth = 0;
};

struct ListRef
{
    std::string id;
    std::string title;
};

struct TaskList
{
    std::string id;
    std::string title;
    std::vector<Task> tasks;
};

struct Snapshot
{
    std::vector<TaskList> lists;
    std::vector<std::string> missing;
};

std::vector<ListRef> parse_lists (std::string_view json, std::string &next);
std::vector<Task> parse_tasks (std::string_view json, std::string &next);

std::vector<Task> order_tasks (std::vector<Task> flat);

std::vector<ListRef> select_lists (const std::vector<ListRef> &all,
                                   const std::vector<std::string> &wanted,
                                   std::vector<std::string> &missing);

std::string snapshot_to_json (const Snapshot &snap,
                              const std::string &selection);
Snapshot snapshot_from_json (std::string_view json, std::string &selection);

class OAuthError : public std::runtime_error
{
public:
    OAuthError (std::string code, const std::string &what)
        : std::runtime_error (what), code_ (std::move (code))
    {
    }
    const std::string &code () const
    {
        return code_;
    }

private:
    std::string code_;
};

struct ClientSecret
{
    std::string client_id;
    std::string client_secret;
    std::string auth_uri = "https://accounts.google.com/o/oauth2/auth";
    std::string token_uri = "https://oauth2.googleapis.com/token";
};

ClientSecret parse_client_secret (std::string_view json);

struct Token
{
    std::string access_token;
    std::string refresh_token;
    util::TimePoint expires_at{};
};

Token parse_token_response (std::string_view json, util::TimePoint now);

std::string parse_token_file (std::string_view json);

void write_token_file (const std::filesystem::path &path, const Token &token);

std::string base64url (std::string_view bytes);

std::string pkce_challenge (std::string_view verifier);

std::string random_token (std::size_t nbytes);

std::string read_file (const std::filesystem::path &path);
}
