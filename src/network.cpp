/* network.cpp -- the Network screen: hosts and services on the LAN.

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

#include "network.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "app.hpp"
#include "modules.hpp"

using json = nlohmann::json;
using namespace std::chrono;

namespace network
{
std::vector<Port>
default_ports ()
{
    return
    {
        {21, "ftp"}, {22, "ssh"}, {23, "telnet"}, {53, "dns"}, {80, "http"},
        {139, "netbios"}, {443, "https"}, {445, "smb"}, {548, "afp"},
        {631, "ipp"}, {1883, "mqtt"}, {3389, "rdp"}, {5000, "upnp"},
        {5900, "vnc"}, {7000, "airplay"}, {7890, "proxy"}, {8080, "http-alt"},
        {8123, "home-assistant"}, {8443, "https-alt"}, {9100, "printer"},
        {32400, "plex"}, {62078, "iphone-sync"},
    };
}

std::string
format_address (std::uint32_t address_bits)
{
    return std::format ("{}.{}.{}.{}", address_bits >> 24, (address_bits >> 16) & 255,
                        (address_bits >> 8) & 255, address_bits & 255);
}

std::uint32_t
parse_address (std::string_view text)
{
    in_addr parsed{};
    if (inet_pton (AF_INET, std::string (text).c_str (), &parsed) != 1)
        throw std::runtime_error (std::format ("bad IPv4 address '{}'", text));
    return ntohl (parsed.s_addr);
}

std::vector<std::uint32_t>
subnet_targets (std::uint32_t address_bits, int prefix_length, int smallest_prefix)
{
    if (prefix_length < smallest_prefix || prefix_length > 32)
        prefix_length = 24;
    if (prefix_length >= 31)
        return {address_bits};
    std::uint32_t mask = prefix_length == 0 ? 0 : ~std::uint32_t{0} << (32 - prefix_length);
    std::uint32_t network_bits = address_bits & mask;
    std::uint32_t broadcast_bits = network_bits | ~mask;
    std::vector<std::uint32_t> targets;
    for (std::uint32_t target = network_bits + 1; target < broadcast_bits; ++target)
        targets.push_back (target);
    return targets;
}

std::map<std::uint32_t, std::string>
parse_arp (std::string_view proc_net_arp)
{
    std::map<std::uint32_t, std::string> neighbours;
    for (std::string_view line : util::split (proc_net_arp, '\n'))
        {
            std::vector<std::string_view> fields;
            for (std::string_view field : util::split (line, ' '))
                if (!field.empty ())
                    fields.push_back (field);
            if (fields.size () < 4 || fields[0] == "IP")
                continue;
            if (fields[2] == "0x0" || fields[3] == "00:00:00:00:00:00")
                continue;
            try
                {
                    neighbours[parse_address (fields[0])] = std::string (fields[3]);
                }
            catch (const std::exception &)
                {
                }
        }
    return neighbours;
}

std::string
ssh_detail (std::string_view banner)
{
    std::string_view line = banner.substr (0, banner.find_first_of ("\r\n"));
    line = util::trim (line);
    if (line.starts_with ("SSH-"))
        {
            auto dash = line.find ('-', 4);
            if (dash != std::string_view::npos)
                line = line.substr (dash + 1);
        }
    return std::string (line.substr (0, 40));
}

std::string
http_detail (std::string_view response)
{
    std::string lowered (response);
    std::ranges::transform (lowered, lowered.begin (), [] (unsigned char letter)
    {
        return static_cast<char> (std::tolower (letter));
    });
    auto server = lowered.find ("\nserver:");
    if (server != std::string::npos)
        {
            std::size_t start = server + 8;
            std::size_t end = response.find_first_of ("\r\n", start);
            return std::string (util::trim (response.substr (start, end - start))
                                .substr (0, 30));
        }
    if (response.starts_with ("HTTP/"))
        {
            auto space = response.find (' ');
            if (space != std::string_view::npos)
                return std::string (response.substr (space + 1, 3));
        }
    return {};
}

std::string
services_text (const std::vector<Service> &services)
{
    std::string text;
    for (const Service &service : services)
        {
            if (!text.empty ())
                text += " · ";
            text += std::format ("{}:{}", service.name, service.port);
            if (!service.detail.empty ())
                text += " (" + service.detail + ")";
        }
    return text;
}

std::string
scan_to_json (const Scan &scan)
{
    json hosts = json::array ();
    for (const Host &host : scan.hosts)
        {
            json services = json::array ();
            for (const Service &service : host.services)
                services.push_back ({{"port", service.port}, {"name", service.name},
                    {"detail", service.detail}
                });
            hosts.push_back ({{"address", host.address}, {"name", host.name},
                {"hostname", host.hostname}, {"mac", host.mac},
                {"self", host.is_self}, {"services", services}
            });
        }
    json document =
    {
        {"interface", scan.interface}, {"subnet", scan.subnet},
        {
            "started", duration_cast<seconds> (scan.started.time_since_epoch ())
            .count ()
        },
        {"took_ms", scan.took.count ()}, {"hosts", hosts}
    };
    return document.dump ();
}

Scan
scan_from_json (std::string_view text)
{
    json document = json::parse (text);
    Scan scan;
    scan.interface = document.value ("interface", "");
    scan.subnet = document.value ("subnet", "");
    scan.started = util::TimePoint{seconds{document.value ("started", 0LL)}};
    scan.took = milliseconds{document.value ("took_ms", 0LL)};
    for (const json &entry : document.at ("hosts"))
        {
            Host host;
            host.address = entry.value ("address", "");
            host.address_bits = parse_address (host.address);
            host.name = entry.value ("name", "");
            host.hostname = entry.value ("hostname", "");
            host.mac = entry.value ("mac", "");
            host.is_self = entry.value ("self", false);
            for (const json &item : entry.value ("services", json::array ()))
                host.services.push_back ({item.value ("port", 0), item.value ("name", ""),
                                          item.value ("detail", "")
                                         });
            scan.hosts.push_back (std::move (host));
        }
    return scan;
}

namespace
{
enum class Answer
{
    silent,
    open,
    refused
};

struct Probe
{
    std::uint32_t target = 0;
    int port = 0;
    int socket_fd = -1;
    steady_clock::time_point deadline;
};

sockaddr_in
socket_address (std::uint32_t target, int port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons (static_cast<std::uint16_t> (port));
    address.sin_addr.s_addr = htonl (target);
    return address;
}

Answer
answer_of (int error_code)
{
    if (error_code == 0)
        return Answer::open;
    if (error_code == ECONNREFUSED)
        return Answer::refused;
    return Answer::silent;
}

std::map<std::pair<std::uint32_t, int>, Answer>
probe_all (const std::vector<std::uint32_t> &targets, const std::vector<Port> &ports,
           milliseconds timeout, std::size_t parallel)
{
    std::map<std::pair<std::uint32_t, int>, Answer> answers;
    std::deque<std::pair<std::uint32_t, int>> queue;
    for (const Port &port : ports)
        for (std::uint32_t target : targets)
            queue.emplace_back (target, port.number);

    std::vector<Probe> in_flight;
    while (!queue.empty () || !in_flight.empty ())
        {
            while (!queue.empty () && in_flight.size () < parallel)
                {
                    auto [target, port] = queue.front ();
                    queue.pop_front ();
                    int socket_fd = socket (AF_INET, SOCK_STREAM | SOCK_NONBLOCK
                                            | SOCK_CLOEXEC, 0);
                    if (socket_fd < 0)
                        {
                            queue.emplace_front (target, port);
                            break;
                        }
                    sockaddr_in address = socket_address (target, port);
                    if (connect (socket_fd, reinterpret_cast<sockaddr *> (&address),
                                 sizeof address) == 0)
                        {
                            answers[ {target, port}] = Answer::open;
                            close (socket_fd);
                        }
                    else if (errno == EINPROGRESS)
                        in_flight.push_back ({target, port, socket_fd,
                                              steady_clock::now () + timeout
                                             });
                    else
                        {
                            answers[ {target, port}] = answer_of (errno);
                            close (socket_fd);
                        }
                }
            if (in_flight.empty ())
                continue;

            std::vector<pollfd> watched;
            for (const Probe &probe : in_flight)
                watched.push_back ({probe.socket_fd, POLLOUT, 0});
            auto now = steady_clock::now ();
            auto nearest = std::ranges::min (in_flight, {}, &Probe::deadline).deadline;
            int wait_ms = static_cast<int> (std::max<long long> (
                                                0, duration_cast<milliseconds> (nearest - now).count ()));
            poll (watched.data (), watched.size (), wait_ms);

            now = steady_clock::now ();
            std::vector<Probe> still_waiting;
            for (std::size_t index = 0; index < in_flight.size (); ++index)
                {
                    Probe &probe = in_flight[index];
                    if (watched[index].revents)
                        {
                            int error_code = 0;
                            socklen_t length = sizeof error_code;
                            getsockopt (probe.socket_fd, SOL_SOCKET, SO_ERROR, &error_code, &length);
                            answers[ {probe.target, probe.port}] = answer_of (error_code);
                            close (probe.socket_fd);
                        }
                    else if (now >= probe.deadline)
                        close (probe.socket_fd);
                    else
                        still_waiting.push_back (probe);
                }
            in_flight = std::move (still_waiting);
        }
    return answers;
}

std::string
exchange (std::uint32_t target, int port, std::string_view request,
          milliseconds timeout)
{
    int socket_fd = socket (AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
        return {};
    auto deadline = steady_clock::now () + timeout;
    auto remaining = [&deadline]
    {
        return static_cast<int> (std::max<long long> (
                                     0, duration_cast<milliseconds> (deadline - steady_clock::now ()).count ()));
    };
    std::string reply;
    sockaddr_in address = socket_address (target, port);
    bool connected = connect (socket_fd, reinterpret_cast<sockaddr *> (&address),
                              sizeof address) == 0;
    if (!connected && errno == EINPROGRESS)
        {
            pollfd watched{socket_fd, POLLOUT, 0};
            int error_code = -1;
            socklen_t length = sizeof error_code;
            if (poll (&watched, 1, remaining ()) == 1)
                getsockopt (socket_fd, SOL_SOCKET, SO_ERROR, &error_code, &length);
            connected = error_code == 0;
        }
    if (connected && !request.empty ())
        connected = send (socket_fd, request.data (), request.size (), MSG_NOSIGNAL)
                    == static_cast<ssize_t> (request.size ());
    while (connected && reply.size () < 2048 && remaining () > 0)
        {
            pollfd watched{socket_fd, POLLIN, 0};
            if (poll (&watched, 1, remaining ()) != 1)
                break;
            char buffer[1024];
            ssize_t received = recv (socket_fd, buffer, sizeof buffer, 0);
            if (received <= 0)
                break;
            reply.append (buffer, static_cast<std::size_t> (received));
            if (request.empty () ? reply.find ('\n') != std::string::npos
                    : reply.find ("\r\n\r\n") != std::string::npos)
                break;
        }
    close (socket_fd);
    return reply;
}

std::string
reverse_name (std::uint32_t target)
{
    sockaddr_in address = socket_address (target, 0);
    char name[NI_MAXHOST] = {};
    if (getnameinfo (reinterpret_cast<sockaddr *> (&address), sizeof address, name,
                     sizeof name, nullptr, 0, NI_NAMEREQD) != 0)
        return {};
    std::string text = name;
    for (std::string_view suffix :
            {".lan", ".local", ".home", ".localdomain"
            })
        if (text.ends_with (suffix))
            text.resize (text.size () - suffix.size ());
    return text;
}

std::string
read_file (const char *path)
{
    std::ifstream input (path);
    return std::string ((std::istreambuf_iterator<char> (input)),
                        std::istreambuf_iterator<char> ());
}

struct Settings
{
    std::vector<Port> ports;
    milliseconds timeout{700};
    std::size_t parallel = 256;
    std::string subnet;
    std::map<std::uint32_t, std::string> known_names;
    std::string self_name;
};

class NetworkSource : public Source
{
public:
    NetworkSource (Settings settings, seconds interval)
        : Source ("network", interval), settings_ (std::move (settings))
    {
    }

    ~NetworkSource () override
    {
        stop ();
    }

    Latest<Scan> scan;

protected:
    void
    fetch () override
    {
        Scan next;
        next.started = util::Clock::now ();
        auto clock_start = steady_clock::now ();

        std::optional<util::Interface> primary = util::primary_ipv4 ();
        std::uint32_t self_bits = primary ? primary->address_bits : 0;
        std::vector<std::uint32_t> targets;
        if (!settings_.subnet.empty ())
            {
                auto slash = settings_.subnet.find ('/');
                std::uint32_t base = parse_address (settings_.subnet.substr (0, slash));
                int prefix = slash == std::string::npos ? 24
                             : std::stoi (settings_.subnet.substr (slash + 1));
                targets = subnet_targets (base, prefix);
                next.subnet = settings_.subnet;
                next.interface = primary ? primary->name : "";
            }
        else
            {
                if (!primary)
                    throw std::runtime_error ("no IPv4 network interface is up");
                targets = subnet_targets (primary->address_bits, primary->prefix_length);
                int prefix = primary->prefix_length >= 22 && primary->prefix_length <= 32
                             ? primary->prefix_length : 24;
                std::uint32_t mask = ~std::uint32_t{0} << (32 - prefix);
                next.subnet = std::format ("{}/{}", format_address (primary->address_bits & mask),
                                           prefix);
                next.interface = primary->name;
            }

        auto answers = probe_all (targets, settings_.ports, settings_.timeout,
                                  settings_.parallel);
        std::map<std::uint32_t, std::string> neighbours = parse_arp (read_file ("/proc/net/arp"));
        std::set<std::uint32_t> target_set (targets.begin (), targets.end ());

        std::map<std::uint32_t, Host> found;
        auto host_for = [&] (std::uint32_t target) -> Host &
        {
            Host &host = found[target];
            host.address_bits = target;
            host.address = format_address (target);
            return host;
        };
        bool local_subnet = self_bits && std::ranges::binary_search (targets, self_bits);
        for (const auto &[key, answer] : answers)
            {
                if (answer == Answer::silent)
                    continue;
                if (local_subnet && key.first != self_bits && !neighbours.contains (key.first))
                    continue;
                Host &host = host_for (key.first);
                if (answer == Answer::open)
                    {
                        auto known = std::ranges::find (settings_.ports, key.second, &Port::number);
                        host.services.push_back ({key.second, known->name, ""});
                    }
            }
        for (const auto &[target, mac] : neighbours)
            if (target_set.contains (target))
                host_for (target).mac = mac;
        if (self_bits && target_set.contains (self_bits))
            host_for (self_bits).is_self = true;

        {
            std::vector<std::jthread> workers;
            for (auto &[target, host] : found)
                workers.emplace_back ([this, &host]
                {
                    describe (host);
                });
        }

        for (auto &[target, host] : found)
            {
                std::ranges::sort (host.services, {}, &Service::port);
                next.hosts.push_back (std::move (host));
            }
        next.took = duration_cast<milliseconds> (steady_clock::now () - clock_start);
        cache_put ("scan.json", scan_to_json (next));
        scan.set (std::move (next));
    }

    std::optional<util::TimePoint>
    restore () override
    {
        auto cached = cache_get ("scan.json");
        if (!cached)
            return std::nullopt;
        Scan restored = scan_from_json (cached->body);
        util::TimePoint started = restored.started;
        scan.set (std::move (restored));
        return started;
    }

private:
    void
    describe (Host &host) const
    {
        if (auto known = settings_.known_names.find (host.address_bits);
                known != settings_.known_names.end ())
            host.name = known->second;
        if (host.name.empty () && host.is_self)
            host.name = settings_.self_name;
        host.hostname = reverse_name (host.address_bits);
        for (Service &service : host.services)
            {
                if (service.port == 22)
                    service.detail = ssh_detail (exchange (host.address_bits, 22, "",
                                                           milliseconds{1500}));
                else if (service.name.starts_with ("http") && !service.name.starts_with ("https"))
                    service.detail = http_detail (exchange (
                                                      host.address_bits, service.port,
                                                      std::format ("HEAD / HTTP/1.0\r\nHost: {}\r\n\r\n",
                                                              host.address),
                                                      milliseconds{1500}));
            }
    }

    Settings settings_;
};

class NetworkScreen : public Screen
{
public:
    explicit NetworkScreen (NetworkSource &source)
        : source_ (source)
    {
    }

    std::string title () const override
    {
        return "Network";
    }

    std::vector<Source *> sources () const override
    {
        return {&source_};
    }

    void
    draw (const tui::Rect &area, util::TimePoint now) override
    {
        auto current = source_.scan.get ();
        std::string heading = "LAN";
        if (current)
            heading = std::format ("{} · {} · {} hosts · scanned {} ago in {:.1f}s",
                                   current->subnet, current->interface,
                                   current->hosts.size (),
                                   util::format_age (current->started, now),
                                   duration<double> (current->took).count ());
        tui::Rect inside = tui::frame (area, heading);
        if (inside.empty ())
            return;
        if (!current)
            {
                tui::put (inside.y, inside.x + 1, "Scanning the local network…",
                          tui::attr (tui::DIM));
                return;
            }

        constexpr int address_width = 16;
        constexpr int mac_width = 19;
        int name_width = std::clamp (inside.w / 5, 12, 28);
        bool show_mac = inside.w >= address_width + name_width + mac_width + 30;
        int services_x = inside.x + 1 + address_width + name_width
                         + (show_mac ? mac_width : 0);
        int services_width = inside.right () - services_x;

        int row = inside.y;
        attr_t header_attr = tui::attr (tui::DIM) | A_BOLD;
        tui::put (row, inside.x + 1, "address", header_attr);
        tui::put (row, inside.x + 1 + address_width, "name", header_attr);
        if (show_mac)
            tui::put (row, inside.x + 1 + address_width + name_width, "MAC", header_attr);
        tui::put (row, services_x, "services", header_attr);
        ++row;

        for (const Host &host : current->hosts)
            {
                if (row >= inside.bottom ())
                    break;
                std::string name = host.name;
                if (name.empty ())
                    name = host.hostname;
                if (host.is_self)
                    name = (name.empty () ? std::string ("this machine") : name) + " ★";
                tui::put (row, inside.x + 1, host.address,
                          tui::attr (host.is_self ? tui::ACCENT : tui::NORMAL));
                tui::put (row, inside.x + 1 + address_width,
                          tui::fit (name.empty () ? "?" : name, name_width - 2),
                          tui::attr (name.empty () ? tui::DIM : tui::TITLE));
                if (show_mac)
                    tui::put (row, inside.x + 1 + address_width + name_width, host.mac,
                              tui::attr (tui::DIM));
                std::string services = services_text (host.services);
                if (services.empty ())
                    tui::put (row, services_x, "no open ports", tui::attr (tui::DIM));
                else
                    tui::put (row, services_x, tui::fit (services, services_width - 1),
                              tui::attr (tui::GOOD));
                ++row;
            }
    }

private:
    NetworkSource &source_;
};
}
}

void
install_network (App &app)
{
    using namespace network;
    const toml::table &table = app.config ().section ("network");
    Settings settings;
    settings.timeout = milliseconds{table["timeout_ms"].value_or (700)};
    settings.parallel = static_cast<std::size_t> (std::clamp (
                            table["parallel"].value_or (256), 1, 900));
    settings.subnet = table["subnet"].value_or<std::string> ("");
    if (const toml::array *ports = table["ports"].as_array ())
        for (const toml::node &node : *ports)
            {
                const toml::table *entry = node.as_table ();
                if (!entry)
                    throw std::runtime_error ("[network] ports must be inline tables "
                                              "like { port = 22, name = \"ssh\" }");
                int number = (*entry)["port"].value_or (0);
                if (number <= 0 || number > 65535)
                    throw std::runtime_error ("[network] ports: bad port number");
                settings.ports.push_back ({number, (*entry)["name"].value_or<std::string> (
                                               std::to_string (number))
                                          });
            }
    else
        settings.ports = default_ports ();

    if (const toml::array *hosts = app.config ().section ("machines")["hosts"].as_array ())
        for (const toml::node &node : *hosts)
            if (const toml::table *host = node.as_table ())
                {
                    std::string address = (*host)["address"].value_or<std::string> ("");
                    std::string name = (*host)["name"].value_or<std::string> ("");
                    if ((*host)["kind"].value_or<std::string> ("") == "local")
                        settings.self_name = name;
                    if (address.empty () || name.empty ())
                        continue;
                    try
                        {
                            settings.known_names[parse_address (address)] = name;
                        }
                    catch (const std::exception &)
                        {
                        }
                }

    auto interval = seconds (table["interval"].value_or (600));
    auto &source = app.add_source (std::make_unique<NetworkSource> (
                                       std::move (settings), interval));
    app.add_screen (std::make_unique<NetworkScreen> (source));
}
