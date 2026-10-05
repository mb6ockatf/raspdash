/* tasks.cpp -- read-only Google Tasks.

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

#include "tasks.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "app.hpp"
#include "http.hpp"
#include "modules.hpp"

using json = nlohmann::json;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace tasks
{
namespace
{
std::string
str (const json &j, const char *key)
{
    auto it = j.find (key);
    return it != j.end () && it->is_string () ? it->get<std::string> () : "";
}

std::optional<year_month_day>
due_date (const std::string &s)
{
    int Y = 0;
    unsigned M = 0, D = 0;
    if (std::sscanf (s.c_str (), "%4d-%2u-%2u", &Y, &M, &D) != 3)
        return std::nullopt;
    year_month_day ymd{year{Y}, month{M}, day{D}};
    if (!ymd.ok ())
        return std::nullopt;
    return ymd;
}

json
parse_json (std::string_view text, std::string_view what)
{
    try
        {
            return json::parse (text);
        }
    catch (const json::parse_error &)
        {
            throw std::runtime_error (std::format ("{}: not valid JSON", what));
        }
}
}

std::vector<ListRef>
parse_lists (std::string_view text, std::string &next)
{
    json j = parse_json (text, "task lists");
    std::vector<ListRef> out;
    if (auto items = j.find ("items"); items != j.end () && items->is_array ())
        for (const json &l : *items)
            out.push_back ({str (l, "id"), str (l, "title")});
    next = str (j, "nextPageToken");
    return out;
}

std::vector<Task>
parse_tasks (std::string_view text, std::string &next)
{
    json j = parse_json (text, "tasks");
    std::vector<Task> out;
    if (auto items = j.find ("items"); items != j.end () && items->is_array ())
        for (const json &t : *items)
            {
                if (t.value ("deleted", false))
                    continue;
                Task task;
                task.id = str (t, "id");
                task.title = str (t, "title");
                task.notes = str (t, "notes");
                task.parent = str (t, "parent");
                task.position = str (t, "position");
                task.completed = str (t, "status") == "completed";
                if (std::string due = str (t, "due"); !due.empty ())
                    task.due = due_date (due);
                out.push_back (std::move (task));
            }
    next = str (j, "nextPageToken");
    return out;
}

std::vector<Task>
order_tasks (std::vector<Task> flat)
{
    std::map<std::string, std::size_t> by_id;
    for (std::size_t i = 0; i < flat.size (); ++i)
        by_id[flat[i].id] = i;

    constexpr std::size_t root = static_cast<std::size_t> (-1);
    std::map<std::size_t, std::vector<std::size_t>> children;
    for (std::size_t i = 0; i < flat.size (); ++i)
        {
            auto p = flat[i].parent.empty () ? by_id.end ()
                     : by_id.find (flat[i].parent);
            std::size_t parent = p == by_id.end () || p->second == i ? root
                                 : p->second;
            children[parent].push_back (i);
        }
    for (auto &[parent, kids] : children)
        std::ranges::sort (kids, [&flat] (std::size_t a, std::size_t b)
        {
            return flat[a].position < flat[b].position;
        });

    std::vector<Task> out;
    out.reserve (flat.size ());
    std::vector<bool> seen (flat.size (), false);
    auto walk = [&] (auto &self, std::size_t parent, int depth) -> void
    {
        auto it = children.find (parent);
        if (it == children.end ())
            return;
        for (std::size_t i : it->second)
            {
                if (seen[i])
                    continue;
                seen[i] = true;
                out.push_back (std::move (flat[i]));
                out.back ().depth = depth;
                self (self, i, depth + 1);
            }
    };
    walk (walk, root, 0);

    for (std::size_t i = 0; i < flat.size (); ++i)
        if (!seen[i])
            {
                seen[i] = true;
                out.push_back (std::move (flat[i]));
                out.back ().depth = 0;
                walk (walk, i, 1);
            }
    return out;
}

std::vector<ListRef>
select_lists (const std::vector<ListRef> &all,
              const std::vector<std::string> &wanted,
              std::vector<std::string> &missing)
{
    if (wanted.empty ())
        return all;
    std::vector<ListRef> out;
    for (const std::string &title : wanted)
        {
            auto it = std::ranges::find (all, title, &ListRef::title);
            if (it == all.end ())
                missing.push_back (title);
            else
                out.push_back (*it);
        }
    return out;
}

ClientSecret
parse_client_secret (std::string_view text)
{
    json j = parse_json (text, "OAuth client file");
    if (j.contains ("web") && !j.contains ("installed"))
        throw std::runtime_error ("the OAuth client is a \"Web application\"; "
                                  "create one of type \"Desktop app\"");
    auto it = j.find ("installed");
    if (it == j.end () || !it->is_object ())
        throw std::runtime_error ("OAuth client file has no \"installed\" "
                                  "section (download the JSON of a Desktop "
                                  "app client)");
    ClientSecret cs;
    cs.client_id = str (*it, "client_id");
    cs.client_secret = str (*it, "client_secret");
    if (std::string u = str (*it, "auth_uri"); !u.empty ())
        cs.auth_uri = u;
    if (std::string u = str (*it, "token_uri"); !u.empty ())
        cs.token_uri = u;
    if (cs.client_id.empty ())
        throw std::runtime_error ("OAuth client file has no client_id");
    return cs;
}

Token
parse_token_response (std::string_view text, util::TimePoint now)
{
    json j;
    try
        {
            j = json::parse (text);
        }
    catch (const json::parse_error &)
        {
            throw OAuthError ("bad_response", std::format (
                                  "token endpoint returned non-JSON: {}",
                                  util::trim (text.substr (0, 120))));
        }
    if (std::string err = str (j, "error"); !err.empty ())
        {
            std::string desc = str (j, "error_description");
            throw OAuthError (err, desc.empty () ? err
                              : std::format ("{}: {}", err, desc));
        }
    Token t;
    t.access_token = str (j, "access_token");
    t.refresh_token = str (j, "refresh_token");
    if (t.access_token.empty ())
        throw OAuthError ("bad_response", "token response has no access_token");
    long ttl = j.value ("expires_in", 3600L);
    t.expires_at = now + seconds{ttl};
    return t;
}

std::string
parse_token_file (std::string_view text)
{
    json j = parse_json (text, "token file");
    std::string r = str (j, "refresh_token");
    if (r.empty ())
        throw std::runtime_error ("token file has no refresh_token; "
                                  "rerun raspdash-auth");
    return r;
}

void
write_token_file (const fs::path &path, const Token &token)
{
    json j;
    j["refresh_token"] = token.refresh_token;
    j["access_token"] = token.access_token;
    j["expires_at"] = duration_cast<seconds> (
                          token.expires_at.time_since_epoch ()).count ();
    j["scope"] = std::string (scope);
    std::string body = j.dump (2) + "\n";

    if (path.has_parent_path ())
        fs::create_directories (path.parent_path ());
    fs::path tmp = path;
    tmp += ".tmp";
    ::unlink (tmp.c_str ());
    int fd = ::open (tmp.c_str (), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                     0600);
    if (fd < 0)
        throw std::runtime_error (std::format ("{}: {}", tmp.string (),
                                               std::strerror (errno)));
    std::size_t done = 0;
    while (done < body.size ())
        {
            ssize_t n = ::write (fd, body.data () + done, body.size () - done);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                {
                    int e = errno;
                    ::close (fd);
                    ::unlink (tmp.c_str ());
                    throw std::runtime_error (std::format ("{}: {}",
                                                           tmp.string (),
                                                           std::strerror (e)));
                }
            done += static_cast<std::size_t> (n);
        }
    if (::fsync (fd) != 0 || ::close (fd) != 0
            || ::rename (tmp.c_str (), path.c_str ()) != 0)
        {
            int e = errno;
            ::unlink (tmp.c_str ());
            throw std::runtime_error (std::format ("{}: {}", path.string (),
                                                   std::strerror (e)));
        }
}

std::string
base64url (std::string_view bytes)
{
    static constexpr char alphabet[]
        = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve ((bytes.size () + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size (); i += 3)
        {
            unsigned v = static_cast<unsigned char> (bytes[i]) << 16
                         | static_cast<unsigned char> (bytes[i + 1]) << 8
                         | static_cast<unsigned char> (bytes[i + 2]);
            out += alphabet[v >> 18 & 63];
            out += alphabet[v >> 12 & 63];
            out += alphabet[v >> 6 & 63];
            out += alphabet[v & 63];
        }
    if (std::size_t rest = bytes.size () - i; rest)
        {
            unsigned v = static_cast<unsigned char> (bytes[i]) << 16;
            if (rest == 2)
                v |= static_cast<unsigned char> (bytes[i + 1]) << 8;
            out += alphabet[v >> 18 & 63];
            out += alphabet[v >> 12 & 63];
            if (rest == 2)
                out += alphabet[v >> 6 & 63];
        }
    return out;
}

std::string
pkce_challenge (std::string_view verifier)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest (verifier.data (), verifier.size (), md, &len,
                    EVP_sha256 (), nullptr) != 1)
        throw std::runtime_error ("SHA-256 failed");
    return base64url (std::string_view (reinterpret_cast<char *> (md), len));
}

std::string
random_token (std::size_t nbytes)
{
    std::string buf (nbytes, '\0');
    if (RAND_bytes (reinterpret_cast<unsigned char *> (buf.data ()),
                    static_cast<int> (nbytes)) != 1)
        throw std::runtime_error ("RAND_bytes failed");
    return base64url (buf);
}

std::string
read_file (const fs::path &path)
{
    std::ifstream in (path, std::ios::binary);
    if (!in)
        throw std::runtime_error (std::format ("cannot read {}",
                                               path.string ()));
    std::ostringstream ss;
    ss << in.rdbuf ();
    return ss.str ();
}

std::string
snapshot_to_json (const Snapshot &snap, const std::string &selection)
{
    json j;
    j["selection"] = selection;
    j["missing"] = snap.missing;
    j["lists"] = json::array ();
    for (const TaskList &l : snap.lists)
        {
            json jl{{"id", l.id}, {"title", l.title}, {"tasks", json::array ()}};
            for (const Task &t : l.tasks)
                {
                    json jt{{"id", t.id}, {"title", t.title}, {"notes", t.notes},
                        {"parent", t.parent}, {"position", t.position},
                        {"completed", t.completed}, {"depth", t.depth}};
                    if (t.due)
                        jt["due"] = std::format ("{:%F}", *t.due);
                    jl["tasks"].push_back (std::move (jt));
                }
            j["lists"].push_back (std::move (jl));
        }
    return j.dump ();
}

Snapshot
snapshot_from_json (std::string_view text, std::string &selection)
{
    json j = parse_json (text, "tasks cache");
    Snapshot snap;
    selection = str (j, "selection");
    for (const json &m : j.at ("missing"))
        snap.missing.push_back (m.get<std::string> ());
    for (const json &jl : j.at ("lists"))
        {
            TaskList l{str (jl, "id"), str (jl, "title"), {}};
            for (const json &jt : jl.at ("tasks"))
                {
                    Task t;
                    t.id = str (jt, "id");
                    t.title = str (jt, "title");
                    t.notes = str (jt, "notes");
                    t.parent = str (jt, "parent");
                    t.position = str (jt, "position");
                    t.completed = jt.value ("completed", false);
                    t.depth = jt.value ("depth", 0);
                    if (std::string due = str (jt, "due"); !due.empty ())
                        t.due = due_date (due);
                    l.tasks.push_back (std::move (t));
                }
            snap.lists.push_back (std::move (l));
        }
    return snap;
}

namespace
{
constexpr std::string_view manual_hint = "see 'info raspdash Tasks'";

struct Settings
{
    std::vector<std::string> lists;
    bool show_completed = false;
    bool show_assigned = false;
    bool show_notes = true;
    int soon_days = 2;
    fs::path client_secret;
    fs::path token;
    std::string api = "https://tasks.googleapis.com/tasks/v1";
    seconds interval{3600};
    const time_zone *tz = nullptr;
};

class TasksSource : public Source
{
public:
    explicit TasksSource (Settings s)
        : Source ("tasks", s.interval), s_ (std::move (s))
    {
    }

    ~TasksSource () override
    {
        stop ();
    }

    Latest<Snapshot> data;

protected:
    void
    fetch () override
    {
        std::vector<ListRef> all;
        std::string next;
        do
            {
                std::string url = s_.api + "/users/@me/lists?maxResults=100";
                if (!next.empty ())
                    url += "&pageToken=" + http::url_encode (next);
                auto page = parse_lists (api_get (url), next);
                all.insert (all.end (), page.begin (), page.end ());
            }
        while (!next.empty ());

        Snapshot snap;
        for (const ListRef &ref : select_lists (all, s_.lists, snap.missing))
            {
                TaskList list{ref.id, ref.title, {}};
                std::vector<Task> flat;
                do
                    {
                        std::string url = std::format (
                                              "{}/lists/{}/tasks?maxResults=100&showCompleted={}"
                                              "&showHidden={}&showAssigned={}", s_.api,
                                              http::url_encode (ref.id), s_.show_completed,
                                              s_.show_completed, s_.show_assigned);
                        if (!next.empty ())
                            url += "&pageToken=" + http::url_encode (next);
                        auto page = parse_tasks (api_get (url), next);
                        flat.insert (flat.end (),
                                     std::make_move_iterator (page.begin ()),
                                     std::make_move_iterator (page.end ()));
                    }
                while (!next.empty ());
                list.tasks = order_tasks (std::move (flat));
                snap.lists.push_back (std::move (list));
            }
        cache_put ("tasks.json", snapshot_to_json (snap, selection ()));
        cached_selection_ = selection ();
        data.set (std::move (snap));
    }

    std::optional<util::TimePoint>
    restore () override
    {
        auto c = cache_get ("tasks.json");
        if (!c)
            return std::nullopt;
        std::string sel;
        Snapshot snap = snapshot_from_json (c->body, sel);
        if (sel != selection ())
            return std::nullopt;
        cached_selection_ = std::move (sel);
        data.set (std::move (snap));
        return c->saved;
    }

    bool
    expired (util::TimePoint) const override
    {
        return cached_selection_ != selection ();
    }

private:
    std::string
    api_get (const std::string &url)
    {
        for (int attempt = 0;; ++attempt)
            {
                ensure_token (attempt > 0);
                http::Request req{url, "GET",
                    {{"Authorization", "Bearer " + token_.access_token}},
                    {}};
                http::Response r = http::perform (req);
                if (r.status == 401 && attempt == 0)
                    continue;
                if (!r.ok ())
                    throw http::Error (std::format (
                                           "Google Tasks HTTP {}: {}", r.status,
                                           util::trim (std::string_view (r.body).substr (0, 160))));
                return std::move (r.body);
            }
    }

    void
    ensure_token (bool force)
    {
        auto now = util::Clock::now ();
        if (!force && !token_.access_token.empty ()
                && now < token_.expires_at - seconds{60})
            return;

        std::error_code ec;
        if (!fs::exists (s_.client_secret, ec))
            throw std::runtime_error (std::format (
                                          "no OAuth client file {}; {}",
                                          s_.client_secret.string (), manual_hint));
        if (!fs::exists (s_.token, ec))
            throw std::runtime_error (std::format (
                                          "no token {}; run raspdash-auth ({})",
                                          s_.token.string (), manual_hint));

        ClientSecret cs = parse_client_secret (read_file (s_.client_secret));
        std::string refresh = parse_token_file (read_file (s_.token));

        http::Response r = http::post_form (cs.token_uri,
        {
            {"grant_type", "refresh_token"},
            {"refresh_token", refresh},
            {"client_id", cs.client_id},
            {"client_secret", cs.client_secret},
        });
        Token t;
        try
            {
                t = parse_token_response (r.body, now);
            }
        catch (const OAuthError &e)
            {
                if (e.code () == "invalid_grant")
                    throw std::runtime_error (std::format (
                                                  "Google token revoked or expired; rerun "
                                                  "raspdash-auth ({})", manual_hint));
                throw;
            }
        if (!t.refresh_token.empty () && t.refresh_token != refresh)
            write_token_file (s_.token, t);
        else
            t.refresh_token = refresh;
        token_ = std::move (t);
    }

    std::string
    selection () const
    {
        std::string out = std::format ("completed={};assigned={};lists=",
                                       s_.show_completed, s_.show_assigned);
        for (const std::string &l : s_.lists)
            out += l + '\x1f';
        return out;
    }

    Settings s_;
    Token token_;
    std::string cached_selection_;
};

class TasksScreen : public Screen
{
public:
    TasksScreen (TasksSource &src, const Settings &s)
        : src_ (src), tz_ (s.tz), show_notes_ (s.show_notes),
          soon_days_ (s.soon_days)
    {
    }

    std::string title () const override
    {
        return "Tasks";
    }
    std::vector<Source *> sources () const override
    {
        return {&src_};
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        auto snap = src_.data.get ();
        if (!snap)
            {
                Source::Status st = src_.status ();
                if (!st.error.empty ())
                    {
                        tui::put (area.y + 1, area.x + 2, "Google Tasks unavailable:",
                                  tui::attr (tui::BAD));
                        tui::put (area.y + 2, area.x + 4,
                                  tui::fit (st.error, area.w - 6),
                                  tui::attr (tui::NORMAL));
                    }
                else
                    tui::put (area.y + 1, area.x + 2, "Loading tasks…",
                              tui::attr (tui::DIM));
                return;
            }
        year_month_day today{floor<days> (util::to_local (now, tz_))};

        tui::Rect body = area;
        if (!snap->missing.empty ())
            {
                std::string m = "List not found: ";
                for (std::size_t i = 0; i < snap->missing.size (); ++i)
                    m += (i ? ", " : "") + snap->missing[i];
                tui::put (body.y, body.x + 1, tui::fit (m, body.w - 2),
                          tui::attr (tui::WARN));
                body = body.below (1);
            }
        if (snap->lists.empty ())
            {
                tui::put (body.y + 1, body.x + 2, "No task lists to show.",
                          tui::attr (tui::DIM));
                return;
            }

        int n = static_cast<int> (snap->lists.size ());
        bool stacked = body.w / n < 30;
        std::vector<tui::Rect> parts = stacked ? body.rows (n) : body.columns (n, 0);
        for (int i = 0; i < n; ++i)
            {
                const TaskList &l = snap->lists[static_cast<std::size_t> (i)];
                long open = std::ranges::count_if (l.tasks, [] (const Task &t)
                {
                    return !t.completed;
                });
                tui::Rect in = tui::frame (parts[static_cast<std::size_t> (i)],
                                           std::format ("{} ({})", l.title, open));
                draw_list (in, l, today);
            }
    }

private:
    void
    draw_list (const tui::Rect &in, const TaskList &l,
               const year_month_day &today) const
    {
        if (in.empty ())
            return;
        if (l.tasks.empty ())
            {
                tui::put (in.y, in.x, "Nothing to do.", tui::attr (tui::GOOD));
                return;
            }
        int y = in.y;
        for (std::size_t i = 0; i < l.tasks.size (); ++i)
            {
                const Task &t = l.tasks[i];
                std::size_t left = l.tasks.size () - i;

                if (y >= in.bottom () || (y == in.bottom () - 1 && left > 1))
                    {
                        if (y < in.bottom ())
                            tui::put (y, in.x, std::format ("… {} more", left),
                                      tui::attr (tui::DIM), in.w);
                        return;
                    }
                int indent = std::min (t.depth * 2, in.w / 3);
                int x = in.x + indent;
                int room = in.w - indent;

                std::string due;
                tui::Role due_role = tui::DIM;
                if (t.due)
                    {
                        auto diff = (sys_days{*t.due} - sys_days{today}).count ();
                        if (diff == 0)
                            due = "today";
                        else if (diff == 1)
                            due = "tomorrow";
                        else
                            due = std::format ("{:%d.%m}", *t.due);
                        if (!t.completed)
                            due_role = diff < 0 ? tui::BAD : diff == 0 ? tui::ACCENT
                                       : diff <= soon_days_ ? tui::WARN : tui::DIM;
                    }
                int dw = due.empty () ? 0 : tui::width (due) + 1;

                attr_t ta = tui::attr (t.completed ? tui::DIM : tui::NORMAL);

                const char *box = t.completed ? tui::icon ("\uf046", "☑")
                                  : tui::icon ("\uf096", "☐");
                tui::put (y, x, box,
                          tui::attr (t.completed ? tui::GOOD : tui::TITLE), room);
                std::string title = t.title.empty () ? "(untitled)" : t.title;
                tui::put (y, x + 2, tui::fit (title, room - 2 - dw), ta);
                if (dw && room - 2 > dw)
                    tui::put (y, in.right () - dw + 1, due, tui::attr (due_role));
                ++y;

                if (show_notes_ && !t.notes.empty () && !t.completed
                        && y < in.bottom () - (left > 1 ? 1 : 0))
                    {
                        std::string first (util::trim (util::split (t.notes, '\n')
                                                       .front ()));
                        if (!first.empty ())
                            tui::put (y++, x + 2, tui::fit (first, room - 2),
                                      tui::attr (tui::DIM));
                    }
            }
    }

    TasksSource &src_;
    const time_zone *tz_;
    bool show_notes_;
    int soon_days_;
};
}
}

void
install_tasks (App &app)
{
    using namespace tasks;
    const Config &cfg = app.config ();
    const toml::table &t = cfg.section ("tasks");
    Settings s;
    if (const toml::array *a = t["lists"].as_array ())
        for (const auto &node : *a)
            if (auto v = node.value<std::string> ())
                s.lists.push_back (*v);
    s.show_completed = t["show_completed"].value_or (false);
    s.show_assigned = t["show_assigned"].value_or (false);
    s.show_notes = t["show_notes"].value_or (true);
    s.soon_days = t["soon_days"].value_or (2);
    s.client_secret = cfg.resolve (t["client_secret"].value_or<std::string> (
                                       "secrets/google_client_secret.json"));
    s.token = cfg.resolve (t["token"].value_or<std::string> (
                               "secrets/google_token.json"));
    s.api = t["api"].value_or<std::string> (std::string (s.api));
    s.interval = seconds (t["interval"].value_or (3600));
    s.tz = util::zone (t["timezone"].value_or<std::string> ("Europe/Moscow"));

    auto &src = app.add_source (std::make_unique<TasksSource> (s));
    app.add_screen (std::make_unique<TasksScreen> (src, s));
}
