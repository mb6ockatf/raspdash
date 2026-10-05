/* machines.cpp -- load, memory and temperatures of machines on the LAN.

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

#include "machines.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "app.hpp"
#include "http.hpp"
#include "modules.hpp"

extern char **environ;

using json = nlohmann::json;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace machines
{
namespace
{
template <typename T>
std::optional<T>
number (std::string_view s)
{
    s = util::trim (s);
    T v{};
    auto r = std::from_chars (s.data (), s.data () + s.size (), v);
    if (r.ec != std::errc () || r.ptr != s.data () + s.size ())
        return std::nullopt;
    return v;
}

std::optional<std::uint64_t>
bytes (std::string_view s)
{
    if (auto u = number<std::uint64_t> (s))
        return u;
    if (auto d = number<double> (s); d && *d >= 0)
        return static_cast<std::uint64_t> (*d);
    return std::nullopt;
}

void
sort_temps (std::vector<Temp> &temps)
{
    std::ranges::stable_sort (temps, std::ranges::greater{}, &Temp::celsius);
}

std::string
digest_hex (const EVP_MD *md, std::string_view data)
{
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!EVP_Digest (data.data (), data.size (), out, &len, md, nullptr))
        throw std::runtime_error ("EVP_Digest failed");
    std::string hex;
    for (unsigned int i = 0; i < len; ++i)
        hex += std::format ("{:02x}", out[i]);
    return hex;
}
}

Stats
parse_stats (std::string_view text)
{
    Stats s;
    bool agent = false;
    for (std::string_view line : util::split (text, '\n'))
        {
            auto eq = line.find ('=');
            if (eq == std::string_view::npos)
                continue;
            std::string_view k = util::trim (line.substr (0, eq));
            std::string_view v = util::trim (line.substr (eq + 1));
            if (k == "agent")
                agent = true;
            else if (k == "hostname")
                s.hostname = v;
            else if (k == "os")
                s.os = v;
            else if (k == "kernel")
                s.kernel = v;
            else if (k == "uptime_s")
                s.uptime_s = number<double> (v);
            else if (k == "ncpu")
                s.ncpu = number<int> (v).value_or (0);
            else if (k == "load1")
                s.load1 = number<double> (v);
            else if (k == "load5")
                s.load5 = number<double> (v);
            else if (k == "load15")
                s.load15 = number<double> (v);
            else if (k == "cpu_pct")
                s.cpu_pct = number<double> (v);
            else if (k == "mem_total")
                s.mem_total = bytes (v);
            else if (k == "mem_avail")
                s.mem_avail = bytes (v);
            else if (k == "swap_total")
                s.swap_total = bytes (v);
            else if (k == "swap_used")
                s.swap_used = bytes (v);
            else if (k == "disk_total")
                s.disk_total = bytes (v);
            else if (k == "disk_used")
                s.disk_used = bytes (v);
            else if (k == "battery_pct")
                s.battery_pct = number<int> (v);
            else if (k == "battery_status")
                s.battery_status = v;
            else if (k.starts_with ("temp."))
                {
                    std::string_view rest = k.substr (5);
                    auto dot = rest.find ('.');
                    Temp t;
                    t.chip = rest.substr (0, dot);
                    if (dot != std::string_view::npos)
                        t.label = rest.substr (dot + 1);
                    auto c = number<double> (v);

                    if (c && *c > 0.5 && *c < 150)
                        {
                            t.celsius = *c;
                            s.temps.push_back (std::move (t));
                        }
                }
        }
    if (!agent)
        {
            std::string_view head = util::trim (text.substr (0, 160));
            throw std::runtime_error (head.empty ()
                                      ? "no output from stats script"
                                      : std::format ("unexpected output: {}",
                                              head));
        }
    sort_temps (s.temps);
    return s;
}

Stats
parse_keenetic (std::string_view system_json, std::string_view version_json)
{
    json j = json::parse (system_json);
    Stats s;
    s.hostname = j.value ("hostname", "");
    if (auto it = j.find ("cpuload"); it != j.end () && it->is_number ())
        s.cpu_pct = it->get<double> ();

    auto kib = [&j] (const char *key) -> std::optional<std::uint64_t>
    {
        auto it = j.find (key);
        if (it == j.end ())
            return std::nullopt;
        if (it->is_number ())
            return it->get<std::uint64_t> () * 1024;
        if (it->is_string ())
            if (auto v = bytes (it->get<std::string> ()))
                return *v * 1024;
        return std::nullopt;
    };
    if (auto total = kib ("memtotal"))
        {
            s.mem_total = total;
            std::uint64_t avail = kib ("memfree").value_or (0)
                                  + kib ("membuffers").value_or (0)
                                  + kib ("memcache").value_or (0);
            s.mem_avail = std::min (avail, *total);
        }
    else if (auto it = j.find ("memory"); it != j.end () && it->is_string ())
        {
            auto parts = util::split (it->get_ref<const std::string &> (), '/');
            if (parts.size () == 2)
                if (auto used = bytes (parts[0]), total = bytes (parts[1]);
                        used && total && *used <= *total)
                    {
                        s.mem_total = *total * 1024;
                        s.mem_avail = (*total - *used) * 1024;
                    }
        }
    if (auto total = kib ("swaptotal"); total && *total > 0)
        {
            s.swap_total = total;
            s.swap_used = *total - std::min (*total, kib ("swapfree").value_or (0));
        }
    if (auto it = j.find ("uptime"); it != j.end ())
        {
            if (it->is_number ())
                s.uptime_s = it->get<double> ();
            else if (it->is_string ())
                s.uptime_s = number<double> (it->get<std::string> ());
        }

    s.os = "KeeneticOS";
    if (!util::trim (version_json).empty ())
        {
            json v = json::parse (version_json);
            std::string release = v.value ("title", v.value ("release", ""));
            std::string model = v.value ("model", v.value ("device", ""));
            if (!release.empty ())
                s.os += " " + release;
            if (!model.empty ())
                s.os = model + " · " + s.os;
        }
    return s;
}

std::string
md5_hex (std::string_view data)
{
    return digest_hex (EVP_md5 (), data);
}

std::string
sha256_hex (std::string_view data)
{
    return digest_hex (EVP_sha256 (), data);
}

std::string
keenetic_password (std::string_view login, std::string_view realm,
                   std::string_view password, std::string_view challenge)
{
    std::string md5 = md5_hex (std::format ("{}:{}:{}", login, realm,
                                            password));
    return sha256_hex (std::string (challenge) + md5);
}

namespace
{
struct Fd
{
    int fd = -1;
    Fd () = default;
    explicit Fd (int f) : fd (f) {}
    Fd (const Fd &) = delete;
    Fd &operator= (const Fd &) = delete;
    ~Fd ()
    {
        reset ();
    }
    void
    reset ()
    {
        if (fd >= 0)
            ::close (fd);
        fd = -1;
    }
};

void
make_pipe (Fd &r, Fd &w)
{
    int p[2];
    if (::pipe2 (p, O_CLOEXEC) != 0)
        throw std::runtime_error (std::format ("pipe: {}",
                                               std::strerror (errno)));
    r.fd = p[0];
    w.fd = p[1];
}
}

CommandResult
run_command (const std::vector<std::string> &argv, std::string_view input,
             milliseconds timeout)
{
    if (argv.empty ())
        throw std::runtime_error ("run_command: empty argv");

    Fd in_r, in_w, out_r, out_w, err_r, err_w;
    make_pipe (in_r, in_w);
    make_pipe (out_r, out_w);
    make_pipe (err_r, err_w);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init (&fa);
    posix_spawn_file_actions_adddup2 (&fa, in_r.fd, 0);
    posix_spawn_file_actions_adddup2 (&fa, out_w.fd, 1);
    posix_spawn_file_actions_adddup2 (&fa, err_w.fd, 2);

    std::vector<char *> cargv;
    for (const std::string &a : argv)
        cargv.push_back (const_cast<char *> (a.c_str ()));
    cargv.push_back (nullptr);

    pid_t pid;
    int rc = posix_spawnp (&pid, cargv[0], &fa, nullptr, cargv.data (),
                           environ);
    posix_spawn_file_actions_destroy (&fa);
    if (rc != 0)
        throw std::runtime_error (std::format ("cannot run {}: {}", argv[0],
                                               std::strerror (rc)));
    in_r.reset ();
    out_w.reset ();
    err_w.reset ();
    for (int fd :
            {
                in_w.fd, out_r.fd, err_r.fd
            })
        ::fcntl (fd, F_SETFL, ::fcntl (fd, F_GETFL) | O_NONBLOCK);

    CommandResult res;
    std::size_t written = 0;
    if (input.empty ())
        in_w.reset ();
    auto deadline = steady_clock::now () + timeout;

    while (in_w.fd >= 0 || out_r.fd >= 0 || err_r.fd >= 0)
        {
            auto left = duration_cast<milliseconds> (deadline
                        - steady_clock::now ());
            if (left <= 0ms)
                {
                    res.timed_out = true;
                    break;
                }
            pollfd fds[3];
            int n = 0;
            if (in_w.fd >= 0)
                fds[n++] = {in_w.fd, POLLOUT, 0};
            if (out_r.fd >= 0)
                fds[n++] = {out_r.fd, POLLIN, 0};
            if (err_r.fd >= 0)
                fds[n++] = {err_r.fd, POLLIN, 0};
            int pr = ::poll (fds, static_cast<nfds_t> (n),
                             static_cast<int> (left.count ()));
            if (pr < 0 && errno != EINTR)
                break;
            for (int i = 0; i < n && pr > 0; ++i)
                {
                    if (!fds[i].revents)
                        continue;
                    if (fds[i].fd == in_w.fd)
                        {
                            ssize_t w = ::write (in_w.fd, input.data () + written,
                                                 input.size () - written);
                            if (w > 0)
                                written += static_cast<std::size_t> (w);

                            if ((w < 0 && errno != EAGAIN && errno != EINTR)
                                    || written >= input.size ())
                                in_w.reset ();
                        }
                    else
                        {
                            Fd &src = fds[i].fd == out_r.fd ? out_r : err_r;
                            std::string &dst = fds[i].fd == out_r.fd ? res.out : res.err;
                            char buf[4096];
                            ssize_t r = ::read (src.fd, buf, sizeof buf);
                            if (r > 0)
                                dst.append (buf, static_cast<std::size_t> (r));
                            else if (r == 0 || (errno != EAGAIN && errno != EINTR))
                                src.reset ();
                        }
                }
        }

    int wst = 0;

    while (!res.timed_out)
        {
            pid_t w = ::waitpid (pid, &wst, WNOHANG);
            if (w == pid)
                break;
            if (w < 0 && errno != EINTR)
                return res;
            if (steady_clock::now () >= deadline)
                res.timed_out = true;
            else
                ::usleep (20000);
        }
    if (res.timed_out)
        {
            ::kill (pid, SIGKILL);
            while (::waitpid (pid, &wst, 0) < 0 && errno == EINTR)
                ;
            return res;
        }
    if (!res.timed_out && WIFEXITED (wst))
        res.status = WEXITSTATUS (wst);
    return res;
}

namespace
{
enum class Kind { local, ssh, keenetic };

struct HostConfig
{
    std::string name;
    Kind kind = Kind::ssh;
    std::string address;
    std::string user;
    int port = 0;
    std::string identity;
    std::vector<std::string> ssh_options;
    fs::path credentials;
    std::string credentials_name;
    seconds interval{10};
    milliseconds timeout{15000};
};

struct Snapshot
{
    Stats stats;
    std::vector<double> cpu_history;
};

std::string
first_line (std::string_view s)
{
    for (std::string_view l : util::split (s, '\n'))
        if (!util::trim (l).empty ())
            return std::string (util::trim (l));
    return {};
}

class HostSource : public Source
{
public:
    HostSource (HostConfig cfg, std::shared_ptr<const std::string> script,
                std::size_t history)
        : Source (cfg.name, cfg.interval, std::min (cfg.interval, seconds{30})),
          cfg_ (std::move (cfg)), script_ (std::move (script)),
          history_max_ (history)
    {
    }

    ~HostSource () override
    {
        stop ();
    }

    const HostConfig &config () const
    {
        return cfg_;
    }

    Latest<Snapshot> snapshot;

    util::TimePoint
    down_since () const
    {
        return down_since_.load ();
    }

protected:

    std::optional<util::TimePoint>
    restore () override
    {
        if (auto c = cache_get ("facts"))
            {
                Stats f = parse_stats (c->body);
                if (cfg_.kind == Kind::keenetic)
                    f.os = util::trim (f.os).empty () ? "KeeneticOS" : f.os;
                snapshot.set (Snapshot{std::move (f), {}});
                facts_ = c->body;
            }
        return std::nullopt;
    }

    void
    fetch () override
    {
        Stats s;
        try
            {
                s = cfg_.kind == Kind::keenetic ? fetch_keenetic () : fetch_script ();
            }
        catch (...)
            {
                if (down_since_.load () == util::TimePoint{})
                    down_since_.store (util::Clock::now ());
                throw;
            }
        down_since_.store (util::TimePoint{});
        std::string facts = std::format ("agent=1\nhostname={}\nos={}\n"
                                         "kernel={}\nncpu={}\n", s.hostname,
                                         s.os, s.kernel, s.ncpu);
        if (facts != facts_)
            {
                cache_put ("facts", facts);
                facts_ = std::move (facts);
            }
        if (s.cpu_pct)
            {
                history_.push_back (*s.cpu_pct);
                while (history_.size () > history_max_)
                    history_.pop_front ();
            }
        snapshot.set (Snapshot{std::move (s), {history_.begin (),
                                               history_.end ()
                                              }
                              });
    }

private:
    Stats
    fetch_script ()
    {
        if (!script_ || script_->empty ())
            throw std::runtime_error ("stats script not found "
                                      "(set [machines] stats_script)");
        std::vector<std::string> argv;
        if (cfg_.kind == Kind::local)
            argv = {"/bin/sh", "-s"};
        else
            {
                argv = {"ssh", "-T",
                        "-o", "BatchMode=yes",
                        "-o", "ConnectTimeout=3",
                        "-o", "ServerAliveInterval=5",
                        "-o", "ServerAliveCountMax=1",
                        "-o", "StrictHostKeyChecking=accept-new",
                        "-o", "LogLevel=ERROR"
                       };
                if (!cfg_.identity.empty ())
                    argv.insert (argv.end (), {"-i", cfg_.identity,
                                               "-o", "IdentitiesOnly=yes"
                                              });
                if (cfg_.port > 0)
                    argv.insert (argv.end (), {"-p", std::to_string (cfg_.port)});
                if (!cfg_.user.empty ())
                    argv.insert (argv.end (), {"-l", cfg_.user});
                for (const std::string &o : cfg_.ssh_options)
                    argv.insert (argv.end (), {"-o", o});

                argv.insert (argv.end (), {"--", cfg_.address, "sh", "-s"});
            }
        CommandResult r = run_command (argv, *script_, cfg_.timeout);
        if (r.timed_out)
            throw std::runtime_error (std::format ("no answer within {}s",
                                                   duration_cast<seconds> (
                                                           cfg_.timeout).count ()));
        if (r.status == 255 && cfg_.kind == Kind::ssh)
            {
                std::string msg = first_line (r.err);
                if (msg.starts_with ("ssh: "))
                    msg = msg.substr (5);
                throw std::runtime_error (msg.empty () ? "ssh failed" : msg);
            }
        if (r.out.find ("agent=") == std::string::npos && r.status != 0)
            {
                std::string msg = first_line (r.err);
                throw std::runtime_error (msg.empty ()
                                          ? std::format ("exit status {}", r.status)
                                          : msg);
            }
        return parse_stats (r.out);
    }

    Stats
    fetch_keenetic ()
    {
        std::string base = cfg_.address.find ("://") == std::string::npos
                           ? "http://" + cfg_.address : cfg_.address;
        if (auto body = rci (base, "/rci/show/system"))
            return parse_keenetic (*body, rci (base, "/rci/show/version")
                                   .value_or (""));
        login (base);
        auto body = rci (base, "/rci/show/system");
        if (!body)
            throw std::runtime_error ("router refused the session after login");
        return parse_keenetic (*body, rci (base, "/rci/show/version")
                               .value_or (""));
    }

    http::Fields
    cookie_header () const
    {
        if (cookie_.empty ())
            return {};
        return {{"Cookie", cookie_}};
    }

    std::optional<std::string>
    rci (const std::string &base, const char *path)
    {
        if (cookie_.empty ())
            return std::nullopt;
        http::Request req{base + path, "GET", cookie_header (), {},
                          seconds{8}, true};
        http::Response r = http::perform (req);
        if (r.status == 401)
            return std::nullopt;
        if (!r.ok ())
            throw std::runtime_error (std::format ("router: HTTP {} for {}",
                                                   r.status, path));
        return std::move (r.body);
    }

    void
    login (const std::string &base)
    {
        std::error_code ec;
        if (cfg_.credentials.empty () || !fs::is_regular_file (cfg_.credentials,
                ec))
            throw std::runtime_error (std::format (
                                          "no router credentials: create {} with "
                                          "login and password",
                                          cfg_.credentials.empty ()
                                          ? std::string ("a credentials file")
                                          : cfg_.credentials_name));
        toml::table cred;
        try
            {
                cred = toml::parse_file (cfg_.credentials.string ());
            }
        catch (const toml::parse_error &e)
            {
                throw std::runtime_error (std::format ("{}: {}",
                                                       cfg_.credentials.string (),
                                                       e.description ()));
            }
        std::string login = cred["login"].value_or<std::string> ("admin");
        std::string password = cred["password"].value_or<std::string> ("");

        cookie_.clear ();
        http::Request req{base + "/auth", "GET", {}, {}, seconds{8}, true};
        http::Response r = http::perform (req);
        if (auto sc = r.header ("set-cookie"))
            cookie_ = sc->substr (0, sc->find (';'));
        if (r.ok ())
            return;
        auto realm = r.header ("x-ndm-realm");
        auto challenge = r.header ("x-ndm-challenge");
        if (r.status != 401 || !realm || !challenge)
            throw std::runtime_error (std::format ("router: unexpected HTTP {} "
                                                   "from /auth (not a Keenetic?)",
                                                   r.status));
        json body = {{"login", login},
            {
                "password",
                keenetic_password (login, *realm, password, *challenge)
            }
        };
        http::Fields h = cookie_header ();
        h.emplace_back ("Content-Type", "application/json");
        http::Request post{base + "/auth", "POST", std::move (h), body.dump (),
                           seconds{8}, true};
        http::Response pr = http::perform (post);
        if (auto sc = pr.header ("set-cookie"))
            cookie_ = sc->substr (0, sc->find (';'));
        if (!pr.ok ())
            {
                cookie_.clear ();
                throw std::runtime_error (std::format (
                                              "router login failed (HTTP {}); check {}",
                                              pr.status, cfg_.credentials_name));
            }
    }

    HostConfig cfg_;
    std::shared_ptr<const std::string> script_;
    std::size_t history_max_;
    std::deque<double> history_;
    std::string cookie_;
    std::string facts_;
    std::atomic<util::TimePoint> down_since_{util::TimePoint{}};
};

constexpr int label_w = 7;

std::string
label (const char *nerd, const char *text)
{
    const char *i = tui::icon (nerd, "");
    return *i ? std::format ("{} {}", i, text) : std::string (text);
}

const char *
logo (Kind kind, const Stats *s)
{
    std::string os = s ? s->os : "";
    if (kind == Kind::keenetic || os.find ("Keenetic") != std::string::npos)
        return tui::icon ("\uF1EB", "");
    if (os.find ("Raspbian") != std::string::npos
            || (s && s->hostname.find ("raspberry") != std::string::npos))
        return tui::icon ("\uF315", "");
    if (os.find ("Devuan") != std::string::npos)
        return tui::icon ("\uF307", "");
    if (os.find ("Debian") != std::string::npos)
        return tui::icon ("\uF306", "");
    if (os.starts_with ("macOS"))
        return tui::icon ("\uF179", "");
    if (!os.empty ())
        return tui::icon ("\uF17C", "");
    return tui::icon ("\uF109", "");
}

std::string
format_uptime (double secs)
{
    long s = static_cast<long> (secs);
    long d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
    if (d > 0)
        return std::format ("{}d {}h", d, h);
    if (h > 0)
        return std::format ("{}h {}m", h, m);
    return std::format ("{}m", m);
}

std::string
temp_label (const Temp &t)
{

    std::string l = t.label;
    if (l.empty () || (l.starts_with ("temp") && l.size () <= 6))
        l = t.chip;
    std::ranges::replace (l, '_', ' ');
    return l;
}

struct Row
{
    int order;
    int priority;
    int height;
    std::function<void (int y, int h)> draw;
    int grow = 0;
};

class MachinesScreen : public Screen
{
public:
    struct Card
    {
        HostConfig cfg;
        HostSource *src;
    };

    explicit MachinesScreen (std::vector<Card> cards)
        : cards_ (std::move (cards))
    {
    }

    std::string title () const override
    {
        return "Machines";
    }

    std::vector<Source *>
    sources () const override
    {
        std::vector<Source *> out;
        for (const Card &c : cards_)
            if (c.src)
                out.push_back (c.src);
        return out;
    }

    std::vector<Source *>
    footer_sources () const override
    {
        std::vector<Source *> answering;
        for (const Card &card : cards_)
            if (card.src && card.src->status ().error.empty ())
                answering.push_back (card.src);
        return answering;
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        if (cards_.empty ())
            {
                tui::put (area.y + 1, area.x + 2, "No hosts in [machines].",
                          tui::attr (tui::DIM));
                return;
            }

        std::vector<const Card *> shown;
        std::vector<std::string> hidden;
        for (const Card &card : cards_)
            {
                if (card.src && card.src->status ().error.empty ())
                    shown.push_back (&card);
                else
                    hidden.push_back (card.cfg.name);
            }
        if (shown.empty ())
            {
                std::string names;
                for (const std::string &name : hidden)
                    names += (names.empty () ? "" : ", ") + name;
                tui::put (area.y + 1, area.x + 2,
                          tui::fit ("All hosts offline: " + names, area.w - 4),
                          tui::attr (tui::DIM));
                return;
            }

        int count = static_cast<int> (shown.size ());
        int max_columns = std::clamp (area.w / 38, 1, count);
        int row_count = (count + max_columns - 1) / max_columns;
        int column_count = (count + row_count - 1) / row_count;
        std::vector<tui::Rect> row_rects = area.rows (row_count, 0);
        for (int index = 0; index < count; ++index)
            {
                int row = index / column_count;
                int in_row = std::min (column_count, count - row * column_count);
                std::vector<tui::Rect> cells = row_rects[row].columns (in_row, 0);
                draw_card (cells[index % column_count], *shown[index], now);
            }
    }

private:
    static void
    status_badge (const tui::Rect &r, const std::string &text, tui::Role role)
    {
        int w = tui::width (text) + 2;
        if (r.w > w + 8)
            tui::put (r.y, r.right () - w - 2, " " + text + " ", tui::attr (role));
    }

    static void
    gauge_row (const tui::Rect &in, int y, const std::string &label, double frac,
               const std::string &value, tui::Role role)
    {
        constexpr int lw = label_w;
        int vw = std::min (tui::width (value), std::max (0, in.w - lw - 4));
        int gw = in.w - lw - vw - 1;
        tui::put (y, in.x, label, tui::attr (tui::DIM), lw);
        if (gw >= 3)
            tui::gauge (y, in.x + lw, gw, frac, tui::attr (role));
        tui::put (y, in.right () - vw, tui::pad (value, vw, 'r'),
                  tui::attr (role));
    }

    void
    draw_card (const tui::Rect &r, const Card &card, util::TimePoint now)
    {
        auto snap = card.src ? card.src->snapshot.get () : nullptr;
        const char *lg = logo (card.cfg.kind, snap ? &snap->stats : nullptr);
        tui::Rect in = tui::frame (r, *lg ? std::format ("{} {}", lg,
                                   card.cfg.name)
                                   : card.cfg.name);
        if (in.empty ())
            return;
        if (!card.src)
            {
                status_badge (r, "not configured", tui::DIM);
                tui::put (in.y, in.x, tui::fit ("no address in config.toml", in.w),
                          tui::attr (tui::DIM));
                return;
            }

        Source::Status st = card.src->status ();
        bool failing = !st.error.empty ();
        if (failing)
            {
                util::TimePoint since = card.src->down_since ();
                status_badge (r, std::format ("{} offline {}", tui::icon ("\uF057", "✗"),
                                              util::format_age (since == util::TimePoint{}
                                                      ? st.error_at : since,
                                                      now)),
                              tui::BAD);
            }
        else if (st.has_data ())
            status_badge (r, std::format ("{} {}", tui::icon ("\uF111", "●"), util::format_age (st.updated,
                                          now)),
                          tui::GOOD);
        else
            status_badge (r, std::format ("{} connecting", tui::icon ("\uF110", "…")), tui::DIM);

        std::vector<Row> rows;
        std::string where = card.cfg.kind == Kind::local ? "local"
                            : card.cfg.address;
        if (failing)
            rows.push_back ({0, 0, 1, [&, in] (int y, int h)
            {
                draw_wrapped (in, y, h, st.error, tui::attr (tui::BAD));
            }, 2
                        });
        if (!snap)
            {
                rows.push_back ({1, 1, 1, [&, in] (int y, int)
                {
                    tui::put (y, in.x, tui::fit (where, in.w), tui::attr (tui::DIM));
                }
                                });
                render_rows (in, rows);
                return;
            }

        const Stats &s = snap->stats;

        auto role = [failing] (tui::Role r)
        {
            return failing ? tui::DIM : r;
        };

        rows.push_back ({1, 3, 1, [&, in] (int y, int)
        {
            std::string line = where;
            if (!s.os.empty ())
                line += " · " + s.os;
            if (s.uptime_s)
                line += " · up " + format_uptime (*s.uptime_s);
            tui::put (y, in.x, tui::fit (line, in.w), tui::attr (tui::DIM));
        }
                        });

        if (s.cpu_pct)
            {
                double f = *s.cpu_pct / 100.0;
                rows.push_back ({2, 0, 1, [&, in, f] (int y, int)
                {
                    gauge_row (in, y, label ("\uF2DB", "CPU"), f, std::format ("{:.0f}%", *s.cpu_pct),
                               role (tui::level (f)));
                }
                                });
            }
        if (snap->cpu_history.size () >= 2)
            rows.push_back ({3, 6, 2, [&, in] (int y, int h)
            {
                draw_history (in, y, h, snap->cpu_history,
                              card.src->config ().interval, role (tui::SERIES1));
            }, 10
                        });
        if (s.load1)
            rows.push_back ({4, 4, 1, [&, in] (int y, int)
            {
                std::string line = std::format ("{:.2f} {:.2f} {:.2f}", *s.load1,
                                                s.load5.value_or (0),
                                                s.load15.value_or (0));
                if (s.ncpu > 0)
                    line += std::format ("  ({} cpu)", s.ncpu);
                double f = s.ncpu > 0 ? *s.load1 / s.ncpu : 0;
                tui::put (y, in.x, label ("\uF0E4", "load"), tui::attr (tui::DIM), label_w);
                tui::put (y, in.x + label_w, tui::fit (line, in.w - label_w),
                          tui::attr (role (tui::level (f, 0.8, 1.5))));
            }
                        });
        if (s.mem_total && s.mem_avail && *s.mem_total > 0)
            {
                std::uint64_t used = *s.mem_total - std::min (*s.mem_avail,
                                     *s.mem_total);
                double f = static_cast<double> (used) / *s.mem_total;
                rows.push_back ({5, 1, 1, [&, in, f, used] (int y, int)
                {
                    gauge_row (in, y, label ("\U000F035B", "RAM"), f,
                               std::format ("{}/{}", util::human_bytes (used),
                                            util::human_bytes (*s.mem_total)),
                               role (tui::level (f, 0.8, 0.92)));
                }
                                });
            }
        if (s.swap_total && *s.swap_total > 0)
            {
                std::uint64_t used = s.swap_used.value_or (0);
                double f = static_cast<double> (used) / *s.swap_total;
                rows.push_back ({6, 7, 1, [&, in, f, used] (int y, int)
                {
                    gauge_row (in, y, label ("\uF0EC", "swap"), f,
                               std::format ("{}/{}", util::human_bytes (used),
                                            util::human_bytes (*s.swap_total)),
                               role (tui::level (f, 0.5, 0.8)));
                }
                                });
            }
        if (s.disk_total && s.disk_used && *s.disk_total > 0)
            {
                double f = static_cast<double> (*s.disk_used) / *s.disk_total;
                rows.push_back ({7, 5, 1, [&, in, f] (int y, int)
                {
                    gauge_row (in, y, label ("\uF0A0", "disk"), f,
                               std::format ("{}/{}", util::human_bytes (*s.disk_used),
                                            util::human_bytes (*s.disk_total)),
                               role (tui::level (f, 0.85, 0.95)));
                }
                                });
            }
        if (!s.temps.empty ())
            rows.push_back ({8, 2, 1, [&, in] (int y, int)
            {
                draw_temps (in, y, s.temps, failing);
            }
                        });
        if (s.battery_pct)
            rows.push_back ({9, 8, 1, [&, in] (int y, int)
            {
                double f = *s.battery_pct / 100.0;
                std::string v = std::format ("{}%", *s.battery_pct);
                if (!s.battery_status.empty ())
                    v += " " + s.battery_status;
                tui::Role r = f < 0.15 ? tui::BAD : f < 0.3 ? tui::WARN : tui::GOOD;
                gauge_row (in, y, label (f > 0.85 ? "\uF240" : f > 0.6 ? "\uF241"
                                         : f > 0.35 ? "\uF242" : f > 0.1 ? "\uF243"
                                         : "\uF244", "bat"), f, v, role (r));
            }
                        });
        render_rows (in, rows);
    }

    static void
    draw_wrapped (const tui::Rect &in, int y, int rows, const std::string &text,
                  attr_t a)
    {
        std::string line;
        auto words = util::split (text, ' ');
        for (std::size_t i = 0; i < words.size () && rows > 0; ++i)
            {
                std::string next = line.empty () ? std::string (words[i])
                                   : line + " " + std::string (words[i]);
                if (tui::width (next) <= in.w || line.empty ())
                    {
                        line = std::move (next);
                        continue;
                    }
                if (rows == 1)
                    {
                        line = std::move (next);
                        break;
                    }
                tui::put (y++, in.x, line, a, in.w);
                --rows;
                line = std::string (words[i]);
            }
        if (rows > 0 && !line.empty ())
            tui::put (y, in.x, tui::fit (line, in.w), a);
    }

    static void
    render_rows (const tui::Rect &in, std::vector<Row> &rows)
    {
        std::vector<Row *> chosen;
        std::vector<Row *> by_prio;
        for (Row &r : rows)
            by_prio.push_back (&r);
        std::ranges::stable_sort (by_prio, {}, &Row::priority);
        int left = in.h;
        for (Row *r : by_prio)
            if (r->height <= left)
                {
                    chosen.push_back (r);
                    left -= r->height;
                }
        std::ranges::sort (chosen, {}, &Row::order);
        int y = in.y;
        for (Row *r : chosen)
            {
                int extra = std::min (r->grow, left);
                left -= extra;
                r->draw (y, r->height + extra);
                y += r->height + extra;
            }
    }

    static void
    draw_history (const tui::Rect &in, int y, int rows,
                  const std::vector<double> &hist, seconds interval,
                  tui::Role role)
    {
        constexpr int lw = label_w;
        int cols = in.w - lw;
        if (cols < 4)
            return;
        tui::put (y, in.x, label ("\uF201", "hist"), tui::attr (tui::DIM), lw);
        tui::Braille b (cols, rows);

        auto shown = std::min<std::size_t> (hist.size (),
                                            static_cast<std::size_t> (b.dots_w ()));
        auto span = interval * static_cast<long> (shown);
        tui::put (y + rows - 1, in.x,
                  span < 120s ? std::format ("{}s", span.count ())
                  : std::format ("{}m", span.count () / 60),
                  tui::attr (tui::DIM), lw - 1);

        int w = b.dots_w (), h = b.dots_h ();
        int n = static_cast<int> (hist.size ());
        for (int x = 0; x < w; ++x)
            {
                int i = n - w + x;
                if (i < 0)
                    continue;
                double v = std::clamp (hist[static_cast<std::size_t> (i)], 0.0, 100.0);
                int top = h - 1 - static_cast<int> (v / 100.0 * (h - 1) + 0.5);
                b.line (x, h - 1, x, top);
            }
        b.draw (y, in.x + lw, tui::attr (role));
    }

    static void
    draw_temps (const tui::Rect &in, int y, const std::vector<Temp> &temps,
                bool stale)
    {
        tui::put (y, in.x, label ("\uF2C9", "temp"), tui::attr (tui::DIM), label_w);
        int x = in.x + label_w;
        std::vector<std::string> seen;
        for (const Temp &t : temps)
            {
                std::string label = temp_label (t);
                if (std::ranges::find (seen, label) != seen.end ())
                    continue;
                seen.push_back (label);
                std::string text = std::format ("{:.0f}° {}", t.celsius, label);
                if (x != in.x + label_w)
                    text = " · " + text;
                int room = in.right () - x;
                if (tui::width (text) > room)
                    break;
                tui::Role r = stale ? tui::DIM
                              : tui::level (t.celsius / 100.0, 0.70, 0.85);

                if (x != in.x + label_w && r == tui::GOOD)
                    r = tui::NORMAL;
                x += tui::put (y, x, text, tui::attr (r));
            }
    }

    std::vector<Card> cards_;
};

Kind
parse_kind (const std::string &k, const std::string &host)
{
    if (k == "local")
        return Kind::local;
    if (k == "ssh")
        return Kind::ssh;
    if (k == "keenetic")
        return Kind::keenetic;
    throw std::runtime_error (std::format ("[machines] host '{}': unknown kind "
                                           "'{}' (local, ssh, keenetic)",
                                           host, k));
}

std::shared_ptr<const std::string>
load_script (const Config &cfg, const toml::table &t)
{
    std::vector<fs::path> candidates;
    if (auto s = t["stats_script"].value<std::string> ())
        candidates.push_back (cfg.resolve (*s));
    else
        {
            if (const char *env = std::getenv ("RASPDASH_STATS_SCRIPT"))
                candidates.emplace_back (env);
            candidates.push_back (cfg.resolve ("agent/raspdash-stats.sh"));
            candidates.push_back (fs::path (PKGDATADIR) / "raspdash-stats.sh");
        }
    for (const fs::path &p : candidates)
        {
            std::ifstream in (p, std::ios::binary);
            if (!in)
                continue;
            std::ostringstream ss;
            ss << in.rdbuf ();
            return std::make_shared<const std::string> (ss.str ());
        }
    return nullptr;
}
}
}

void
install_machines (App &app)
{
    using namespace machines;
    const Config &cfg = app.config ();
    const toml::table &t = cfg.section ("machines");

    auto script = load_script (cfg, t);
    seconds interval{t["interval"].value_or (10)};
    milliseconds timeout{static_cast<long> (t["timeout"].value_or (15.0)
                                            * 1000)};
    std::size_t history = static_cast<std::size_t> (t["history"].value_or (120));
    std::string identity = t["ssh_identity"].value_or<std::string> ("");

    std::vector<MachinesScreen::Card> cards;
    const toml::array *hosts = t["hosts"].as_array ();
    if (hosts)
        for (const toml::node &node : *hosts)
            {
                const toml::table *h = node.as_table ();
                if (!h)
                    throw std::runtime_error ("[[machines.hosts]] entries must be "
                                              "tables");
                HostConfig hc;
                hc.name = (*h)["name"].value_or<std::string> ("");
                hc.address = (*h)["address"].value_or<std::string> ("");
                if (hc.name.empty ())
                    hc.name = hc.address.empty () ? "?" : hc.address;
                hc.kind = parse_kind ((*h)["kind"].value_or<std::string> ("ssh"),
                                      hc.name);
                hc.user = (*h)["user"].value_or<std::string> ("");
                hc.port = (*h)["port"].value_or (0);
                std::string id = (*h)["identity"].value_or<std::string> (std::string (identity));
                if (!id.empty ())
                    hc.identity = cfg.resolve (id).string ();
                if (const toml::array *opts = (*h)["ssh_options"].as_array ())
                    for (const toml::node &o : *opts)
                        if (auto s = o.value<std::string> ())
                            hc.ssh_options.push_back (*s);
                if (auto c = (*h)["credentials"].value<std::string> ())
                    {
                        hc.credentials = cfg.resolve (*c);
                        hc.credentials_name = *c;
                    }
                hc.interval = seconds{(*h)["interval"].value_or (interval.count ())};
                hc.timeout = timeout;
                if (auto to = (*h)["timeout"].value<double> ())
                    hc.timeout = milliseconds{static_cast<long> (*to * 1000)};

                bool configured = hc.kind == Kind::local || !hc.address.empty ();
                HostSource *src = nullptr;
                if (configured && (*h)["enabled"].value_or (true))
                    src = &app.add_source (std::make_unique<HostSource> (hc, script,
                                           history));
                cards.push_back ({std::move (hc), src});
            }
    app.add_screen (std::make_unique<MachinesScreen> (std::move (cards)));
}
