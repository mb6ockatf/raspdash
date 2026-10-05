/* http.cpp -- minimal blocking HTTP client on top of libcurl.

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

#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>

#include <curl/curl.h>

#include "util.hpp"

namespace http
{
namespace
{
std::string user_agent_;

std::size_t
on_body (char *data, std::size_t size, std::size_t n, void *user)
{
    static_cast<std::string *> (user)->append (data, size * n);
    return size * n;
}

std::size_t
on_header (char *data, std::size_t size, std::size_t n, void *user)
{
    auto *resp = static_cast<Response *> (user);
    std::string_view line (data, size * n);

    if (line.starts_with ("HTTP/"))
        resp->headers.clear ();
    else if (auto colon = line.find (':'); colon != std::string_view::npos)
        {
            std::string name (util::trim (line.substr (0, colon)));
            std::ranges::transform (name, name.begin (), [] (unsigned char c)
            {
                return std::tolower (c);
            });
            resp->headers.emplace_back (
                std::move (name), std::string (util::trim (line.substr (colon + 1))));
        }
    return size * n;
}

struct CurlDeleter
{
    void operator() (CURL *c) const
    {
        curl_easy_cleanup (c);
    }
};

struct SlistDeleter
{
    void operator() (curl_slist *l) const
    {
        curl_slist_free_all (l);
    }
};
}

std::optional<std::string>
Response::header (std::string_view name) const
{
    for (const auto &[k, v] : headers)
        if (k == name)
            return v;
    return std::nullopt;
}

void
global_init (std::string user_agent)
{
    curl_global_init (CURL_GLOBAL_DEFAULT);
    user_agent_ = std::move (user_agent);
}

void
global_cleanup ()
{
    curl_global_cleanup ();
}

Response
perform (const Request &req)
{
    std::unique_ptr<CURL, CurlDeleter> curl (curl_easy_init ());
    if (!curl)
        throw Error ("curl_easy_init failed");
    CURL *c = curl.get ();

    Response resp;
    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt (c, CURLOPT_URL, req.url.c_str ());
    curl_easy_setopt (c, CURLOPT_USERAGENT, user_agent_.c_str ());
    curl_easy_setopt (c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt (c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt (c, CURLOPT_TIMEOUT, static_cast<long> (req.timeout.count ()));
    curl_easy_setopt (c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt (c, CURLOPT_NOSIGNAL, 1L);
    if (req.direct)
        curl_easy_setopt (c, CURLOPT_NOPROXY, "*");
    curl_easy_setopt (c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt (c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt (c, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt (c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt (c, CURLOPT_HEADERDATA, &resp);

    std::unique_ptr<curl_slist, SlistDeleter> hdrs;
    for (const auto &[k, v] : req.headers)
        {
            std::string line = std::format ("{}: {}", k, v);
            hdrs.reset (curl_slist_append (hdrs.release (), line.c_str ()));
        }
    if (hdrs)
        curl_easy_setopt (c, CURLOPT_HTTPHEADER, hdrs.get ());

    if (req.method == "POST")
        {
            curl_easy_setopt (c, CURLOPT_POST, 1L);
            curl_easy_setopt (c, CURLOPT_POSTFIELDSIZE,
                              static_cast<long> (req.body.size ()));
            curl_easy_setopt (c, CURLOPT_POSTFIELDS, req.body.c_str ());
        }
    else if (req.method != "GET")
        {
            curl_easy_setopt (c, CURLOPT_CUSTOMREQUEST, req.method.c_str ());
            if (!req.body.empty ())
                {
                    curl_easy_setopt (c, CURLOPT_POSTFIELDSIZE,
                                      static_cast<long> (req.body.size ()));
                    curl_easy_setopt (c, CURLOPT_POSTFIELDS, req.body.c_str ());
                }
        }

    CURLcode rc = curl_easy_perform (c);
    if (rc != CURLE_OK)
        throw Error (std::format ("{}: {}", req.url,
                                  *errbuf ? errbuf : curl_easy_strerror (rc)));
    curl_easy_getinfo (c, CURLINFO_RESPONSE_CODE, &resp.status);
    return resp;
}

std::string
get (std::string url, Fields headers)
{
    Request req{std::move (url), "GET", std::move (headers), {}};
    Response r = perform (req);
    if (!r.ok ())
        throw Error (std::format ("HTTP {} from {}: {}", r.status, req.url,
                                  util::trim (std::string_view (r.body)
                                              .substr (0, 120))));
    return std::move (r.body);
}

Response
post_form (std::string url, const Fields &form, Fields headers)
{
    headers.emplace_back ("Content-Type", "application/x-www-form-urlencoded");
    Request req{std::move (url), "POST", std::move (headers),
                form_encode (form)};
    return perform (req);
}

std::string
url_encode (std::string_view s)
{
    std::string out;
    for (unsigned char c : s)
        if (std::isalnum (c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char> (c);
        else
            out += std::format ("%{:02X}", c);
    return out;
}

std::string
form_encode (const Fields &fields)
{
    std::string out;
    for (const auto &[k, v] : fields)
        {
            if (!out.empty ())
                out += '&';
            out += url_encode (k);
            out += '=';
            out += url_encode (v);
        }
    return out;
}
}
