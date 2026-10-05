/* http.hpp -- minimal blocking HTTP client on top of libcurl.

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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace http
{
using Fields = std::vector<std::pair<std::string, std::string>>;

struct Request
{
    std::string url;
    std::string method = "GET";
    Fields headers;
    std::string body;
    std::chrono::seconds timeout{20};
    bool direct = false;
};

struct Response
{
    long status = 0;
    std::string body;
    Fields headers;

    std::optional<std::string> header (std::string_view name) const;
    bool ok () const
    {
        return status >= 200 && status < 300;
    }
};

class Error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

void global_init (std::string user_agent);
void global_cleanup ();

Response perform (const Request &req);

std::string get (std::string url, Fields headers = {});

Response post_form (std::string url, const Fields &form,
                    Fields headers = {});

std::string url_encode (std::string_view s);
std::string form_encode (const Fields &fields);
}
