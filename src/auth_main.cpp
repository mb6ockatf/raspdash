/* auth_main.cpp -- raspdash-auth: one-time Google OAuth authorization.

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

#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <map>
#include <print>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.hpp"
#include "http.hpp"
#include "tasks.hpp"

namespace fs = std::filesystem;

namespace
{
void
usage (std::FILE *out)
{
    std::println (out, "Usage: raspdash-auth [OPTION]...\n"
                  "Authorize raspdash to read your Google Tasks and save "
                  "the token.\n"
                  "\n"
                  "Run it on a computer with a web browser.  It prints a "
                  "Google consent URL,\n"
                  "waits for the browser to come back to a local port, "
                  "and writes the token\n"
                  "file named in the [tasks] section of the configuration.\n"
                  "\n"
                  "  -c, --config=FILE          read configuration from FILE\n"
                  "      --client-secret=FILE   OAuth client JSON "
                  "(default: from config)\n"
                  "      --token=FILE           where to write the token "
                  "(default: from config)\n"
                  "  -p, --port=PORT            listen on PORT instead of a "
                  "random one\n"
                  "      --no-browser           do not try to open a browser\n"
                  "  -h, --help                 display this help and exit\n"
                  "  -V, --version              output version information "
                  "and exit\n"
                  "\n"
                  "Report bugs to: <{}>\n"
                  "{} home page: <{}>",
                  PACKAGE_BUGREPORT, PACKAGE_NAME, PACKAGE_URL);
}

void
version ()
{
    std::println ("raspdash-auth ({}) {}\n"
                  "Copyright (C) 2026 mb6ockatf\n"
                  "License GPLv3+: GNU GPL version 3 or later "
                  "<https://gnu.org/licenses/gpl.html>.\n"
                  "This is free software: you are free to change and "
                  "redistribute it.\n"
                  "There is NO WARRANTY, to the extent permitted by law.",
                  PACKAGE_NAME, PACKAGE_VERSION);
}

bool
option (int argc, char **argv, int &i, std::string_view lng, char shrt,
        std::string &value)
{
    std::string_view a = argv[i];
    std::string prefix = std::string ("--") + std::string (lng);
    if (a == prefix || (shrt && a == std::string{'-', shrt}))
        {
            if (i + 1 >= argc)
                throw std::runtime_error (std::format ("option '{}' requires "
                                                       "an argument", a));
            value = argv[++i];
            return true;
        }
    if (a.starts_with (prefix + "="))
        {
            value = a.substr (prefix.size () + 1);
            return true;
        }
    if (shrt && a.size () > 2 && a[0] == '-' && a[1] == shrt)
        {
            value = a.substr (2);
            return true;
        }
    return false;
}

class Fd
{
public:
    explicit Fd (int fd = -1) : fd_ (fd) {}
    ~Fd ()
    {
        if (fd_ >= 0)
            ::close (fd_);
    }
    Fd (const Fd &) = delete;
    Fd &operator= (const Fd &) = delete;
    int get () const
    {
        return fd_;
    }

private:
    int fd_;
};

std::runtime_error
sys_error (std::string_view what)
{
    return std::runtime_error (std::format ("{}: {}", what,
                                            std::strerror (errno)));
}

int
hex (char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string
url_decode (std::string_view s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size (); ++i)
        if (s[i] == '+')
            out += ' ';
        else if (s[i] == '%' && i + 2 < s.size ()
                 && hex (s[i + 1]) >= 0 && hex (s[i + 2]) >= 0)
            {
                out += static_cast<char> (hex (s[i + 1]) * 16 + hex (s[i + 2]));
                i += 2;
            }
        else
            out += s[i];
    return out;
}

std::map<std::string, std::string>
query_params (std::string_view target)
{
    std::map<std::string, std::string> out;
    auto q = target.find ('?');
    if (q == std::string_view::npos)
        return out;
    for (std::string_view kv : util::split (target.substr (q + 1), '&'))
        {
            auto eq = kv.find ('=');
            if (eq == std::string_view::npos)
                out[url_decode (kv)] = "";
            else
                out[url_decode (kv.substr (0, eq))] = url_decode (kv.substr (eq + 1));
        }
    return out;
}

void
reply (int fd, int status, std::string_view text)
{
    std::string body = std::format (
                           "<!doctype html><meta charset=utf-8><title>raspdash</title>"
                           "<body style=\"font-family:sans-serif;margin:3em\">"
                           "<h2>raspdash</h2><p>{}</p></body>\n", text);
    std::string resp = std::format ("HTTP/1.1 {} {}\r\n"
                                    "Content-Type: text/html; charset=utf-8\r\n"
                                    "Content-Length: {}\r\n"
                                    "Connection: close\r\n\r\n{}",
                                    status, status == 200 ? "OK" : "Not Found",
                                    body.size (), body);
    std::size_t done = 0;
    while (done < resp.size ())
        {
            ssize_t n = ::send (fd, resp.data () + done, resp.size () - done,
                                MSG_NOSIGNAL);
            if (n <= 0)
                break;
            done += static_cast<std::size_t> (n);
        }
}

void
open_browser (const std::string &url)
{
    pid_t pid = ::fork ();
    if (pid == 0)
        {
            int null = ::open ("/dev/null", O_RDWR);
            if (null >= 0)
                {
                    ::dup2 (null, 0);
                    ::dup2 (null, 1);
                    ::dup2 (null, 2);
                }
            ::execlp ("xdg-open", "xdg-open", url.c_str (),
                      static_cast<char *> (nullptr));
            ::_exit (127);
        }
}

std::string
receive_code (int listener, const std::string &state)
{
    constexpr int timeout_ms = 10 * 60 * 1000;
    for (;;)
        {
            pollfd p{listener, POLLIN, 0};
            int rc = ::poll (&p, 1, timeout_ms);
            if (rc < 0 && errno == EINTR)
                continue;
            if (rc < 0)
                throw sys_error ("poll");
            if (rc == 0)
                throw std::runtime_error ("timed out waiting for the browser");

            Fd conn (::accept (listener, nullptr, nullptr));
            if (conn.get () < 0)
                continue;
            std::string req;
            char buf[4096];
            while (req.find ("\r\n\r\n") == std::string::npos && req.size () < 65536)
                {
                    pollfd cp{conn.get (), POLLIN, 0};
                    if (::poll (&cp, 1, 5000) <= 0)
                        break;
                    ssize_t n = ::recv (conn.get (), buf, sizeof buf, 0);
                    if (n <= 0)
                        break;
                    req.append (buf, static_cast<std::size_t> (n));
                }

            auto line_end = req.find ("\r\n");
            std::string_view line = std::string_view (req).substr (0, line_end);
            auto parts = util::split (line, ' ');
            if (parts.size () < 2 || parts[0] != "GET")
                {
                    reply (conn.get (), 404, "Unexpected request.");
                    continue;
                }
            auto params = query_params (parts[1]);
            if (!params.contains ("code") && !params.contains ("error"))
                {

                    reply (conn.get (), 404, "Not found.");
                    continue;
                }
            if (params["state"] != state)
                {
                    reply (conn.get (), 200, "State mismatch: this response "
                           "was not for this run.  Start raspdash-auth again.");
                    throw std::runtime_error ("OAuth state mismatch; ignoring "
                                              "the response");
                }
            if (params.contains ("error"))
                {
                    reply (conn.get (), 200, "Authorization was not granted.  "
                           "You can close this tab.");
                    throw std::runtime_error (std::format (
                                                  "authorization refused: {}", params["error"]));
                }
            reply (conn.get (), 200, "Authorized.  You can close this tab and "
                   "return to the terminal.");
            return params["code"];
        }
}

int
authorize (const fs::path &client_file, const fs::path &token_file, int port,
           bool browser)
{
    tasks::ClientSecret cs
        = tasks::parse_client_secret (tasks::read_file (client_file));

    Fd listener (::socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (listener.get () < 0)
        throw sys_error ("socket");
    int one = 1;
    ::setsockopt (listener.get (), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    addr.sin_port = htons (static_cast<std::uint16_t> (port));
    if (::bind (listener.get (), reinterpret_cast<sockaddr *> (&addr),
                sizeof addr) != 0)
        throw sys_error (std::format ("bind 127.0.0.1:{}", port));
    if (::listen (listener.get (), 4) != 0)
        throw sys_error ("listen");
    socklen_t len = sizeof addr;
    ::getsockname (listener.get (), reinterpret_cast<sockaddr *> (&addr), &len);
    int bound = ntohs (addr.sin_port);

    std::string redirect = std::format ("http://127.0.0.1:{}", bound);
    std::string verifier = tasks::random_token (48);
    std::string state = tasks::random_token (16);
    std::string url = cs.auth_uri + "?" + http::form_encode (
    {
        {"client_id", cs.client_id},
        {"redirect_uri", redirect},
        {"response_type", "code"},
        {"scope", std::string (tasks::scope)},
        {"code_challenge", tasks::pkce_challenge (verifier)},
        {"code_challenge_method", "S256"},
        {"state", state},
        {"access_type", "offline"},
        {"prompt", "consent"},
    });

    std::println ("Open this URL in a browser and allow read-only access to "
                  "Google Tasks:\n\n  {}\n", url);
    std::println ("Waiting for the browser on {} ...", redirect);
    std::fflush (stdout);
    if (browser)
        open_browser (url);

    std::string code = receive_code (listener.get (), state);

    http::Response r = http::post_form (cs.token_uri,
    {
        {"code", code},
        {"client_id", cs.client_id},
        {"client_secret", cs.client_secret},
        {"redirect_uri", redirect},
        {"grant_type", "authorization_code"},
        {"code_verifier", verifier},
    });
    tasks::Token t = tasks::parse_token_response (r.body, util::Clock::now ());
    if (t.refresh_token.empty ())
        throw std::runtime_error ("Google returned no refresh token; revoke "
                                  "raspdash at https://myaccount.google.com/"
                                  "permissions and run raspdash-auth again");
    tasks::write_token_file (token_file, t);

    while (::waitpid (-1, nullptr, WNOHANG) > 0)
        ;
    std::println ("\nToken saved to {} (mode 0600).", token_file.string ());
    std::println ("Copy it to the Pi next to raspdash's config, e.g.:\n"
                  "  scp {} pi:raspdash/secrets/", token_file.string ());
    return 0;
}
}

int
main (int argc, char **argv)
{
    std::setlocale (LC_ALL, "");
    std::string config_arg, client_arg, token_arg, port_arg;
    bool browser = true;
    try
        {
            for (int i = 1; i < argc; ++i)
                {
                    std::string_view a = argv[i];
                    if (a == "-h" || a == "--help")
                        {
                            usage (stdout);
                            return 0;
                        }
                    if (a == "-V" || a == "--version")
                        {
                            version ();
                            return 0;
                        }
                    if (a == "--no-browser")
                        browser = false;
                    else if (!option (argc, argv, i, "config", 'c', config_arg)
                             && !option (argc, argv, i, "client-secret", 0,
                                         client_arg)
                             && !option (argc, argv, i, "token", 0, token_arg)
                             && !option (argc, argv, i, "port", 'p', port_arg))
                        throw std::runtime_error (std::format (
                                                      "unrecognized option '{}'", a));
                }
        }
    catch (const std::exception &e)
        {
            std::println (stderr, "raspdash-auth: {}", e.what ());
            std::println (stderr, "Try 'raspdash-auth --help' for more "
                          "information.");
            return 2;
        }

    try
        {
            int port = 0;
            if (!port_arg.empty ())
                {
                    char *end = nullptr;
                    long p = std::strtol (port_arg.c_str (), &end, 10);
                    if (*end || p < 0 || p > 65535)
                        throw std::runtime_error (std::format ("bad port '{}'",
                                                               port_arg));
                    port = static_cast<int> (p);
                }

            fs::path client_file = client_arg, token_file = token_arg;
            if (client_file.empty () || token_file.empty ())
                {
                    auto path = Config::find (config_arg);
                    if (!path)
                        throw std::runtime_error ("no configuration file found; "
                                                  "use --config, or both "
                                                  "--client-secret and --token");
                    Config cfg = Config::load (*path);
                    const toml::table &t = cfg.section ("tasks");
                    if (client_file.empty ())
                        client_file = cfg.resolve (
                                          t["client_secret"].value_or<std::string> (
                                              "secrets/google_client_secret.json"));
                    if (token_file.empty ())
                        token_file = cfg.resolve (t["token"].value_or<std::string> (
                                                      "secrets/google_token.json"));
                }

            http::global_init (PACKAGE_NAME "-auth/" PACKAGE_VERSION);
            int status = authorize (client_file, token_file, port, browser);
            http::global_cleanup ();
            return status;
        }
    catch (const std::exception &e)
        {
            std::println (stderr, "raspdash-auth: {}", e.what ());
            return 1;
        }
}
