/* main.cpp -- raspdash entry point.

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

#include <clocale>
#include <csignal>
#include <cstring>
#include <exception>
#include <format>
#include <print>
#include <string>
#include <string_view>

#include "app.hpp"
#include "config.hpp"
#include "http.hpp"
#include "modules.hpp"

namespace
{
void
usage (std::FILE *out)
{
    std::println (out, "Usage: raspdash [OPTION]...\n"
                  "Full-screen terminal dashboard for a Raspberry Pi.\n"
                  "\n"
                  "  -c, --config=FILE    read configuration from FILE\n"
                  "  -s, --screen=NAME    start on screen NAME (its tab title)\n"
                  "      --check          fetch every source once, report, exit\n"
                  "  -h, --help           display this help and exit\n"
                  "  -V, --version        output version information and exit\n"
                  "\n"
                  "Without --config, raspdash reads $RASPDASH_CONFIG,\n"
                  "./config.toml, ~/.config/raspdash/config.toml or\n"
                  "{}/raspdash/config.toml, whichever exists first.\n"
                  "\n"
                  "Report bugs to: <{}>\n"
                  "{} home page: <{}>",
                  SYSCONFDIR, PACKAGE_BUGREPORT, PACKAGE_NAME, PACKAGE_URL);
}

void
version ()
{
    std::println ("{}\n"
                  "Copyright (C) 2026 mb6ockatf\n"
                  "License GPLv3+: GNU GPL version 3 or later "
                  "<https://gnu.org/licenses/gpl.html>.\n"
                  "This is free software: you are free to change and "
                  "redistribute it.\n"
                  "There is NO WARRANTY, to the extent permitted by law.",
                  PACKAGE_STRING);
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
                throw std::runtime_error (std::format ("option '{}' requires an "
                                                       "argument", a));
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
}

int
main (int argc, char **argv)
{
    std::setlocale (LC_ALL, "");
    std::signal (SIGPIPE, SIG_IGN);
    std::string config_arg, screen_arg;
    bool check = false;
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
                    if (a == "--check")
                        check = true;
                    else if (!option (argc, argv, i, "config", 'c', config_arg)
                             && !option (argc, argv, i, "screen", 's', screen_arg))
                        throw std::runtime_error (std::format ("unrecognized option '{}'",
                                                               a));
                }
        }
    catch (const std::exception &e)
        {
            std::println (stderr, "raspdash: {}", e.what ());
            std::println (stderr, "Try 'raspdash --help' for more information.");
            return 2;
        }

    try
        {
            auto path = Config::find (config_arg);
            if (!path)
                throw std::runtime_error ("no configuration file found "
                                          "(see 'raspdash --help')");
            Config cfg = Config::load (*path);
            http::global_init (cfg.user_agent ());

            App app (cfg);
            const toml::array *wanted = cfg.section ("app")["screens"].as_array ();
            auto install = [&app] (std::string_view name)
            {
                for (const Module &m : modules)
                    if (m.name == name)
                        return m.install (app);
                throw std::runtime_error (std::format ("unknown screen '{}' in "
                                                       "[app] screens", name));
            };
            if (wanted)
                for (const auto &node : *wanted)
                    install (node.value_or<std::string_view> (""));
            else
                for (const Module &m : modules)
                    m.install (app);

            int status = check ? app.check () : app.run (screen_arg);
            app.stop_sources ();
            http::global_cleanup ();
            return status;
        }
    catch (const std::exception &e)
        {
            std::println (stderr, "raspdash: {}", e.what ());
            return 1;
        }
}
