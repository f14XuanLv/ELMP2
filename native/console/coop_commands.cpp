// Coop console commands (P2): `elmp lan host|join|leave|status`. The manager's net page calls the same
// functions (F2.1).

#include "console/console.h"

#include "coop/client.h"
#include "coop/commands.h"
#include "coop/host.h"
#include "coop/session.h"
#include "coop/maps.h"
#include "world/level_instance.h"
#include "modes/hunter.h"
#include "sdk/sdk.h"

#include <charconv>
#include <format>
#include <stdexcept>

namespace elmp::console {

namespace {

std::uint16_t port_of(const std::string& s) {
    unsigned v = 0;
    if (std::from_chars(s.data(), s.data() + s.size(), v).ec != std::errc{} || v == 0 || v > 65535) throw std::runtime_error("bad port '" + s + "'");
    return static_cast<std::uint16_t>(v);
}

void lan_cmd(Args a, std::string& out) {
    const std::string sub = a.empty() ? "status" : a[0];
    if (sub == "host") { // host <code> [port] [maxGuests]
        if (a.size() < 2) throw std::runtime_error("usage: elmp lan host <code> [port] [maxGuests]");
        const auto port = a.size() > 2 ? port_of(a[2]) : std::uint16_t{7777};
        const std::size_t max = a.size() > 3 ? std::stoul(a[3]) : 3;
        if (auto e = coop::start_host(port, a[1], max); !e.empty()) throw std::runtime_error(e);
        out += std::format("hosting on UDP {} (max {} guests)\n", port, max);
    } else if (sub == "join") { // join <addr> <code> [port]
        if (a.size() < 3) throw std::runtime_error("usage: elmp lan join <addr> <code> [port]");
        const auto port = a.size() > 3 ? port_of(a[3]) : std::uint16_t{7777};
        if (auto e = coop::start_join(a[1], port, a[2]); !e.empty()) throw std::runtime_error(e);
        out += std::format("joining {}:{}\n", a[1], port);
    } else if (sub == "leave") {
        coop::leave("left by command");
        out += "left\n";
    } else if (sub == "bg" && a.size() > 2 && a[1] == "load") { // Dev / P3: background map instance
        const float y = a.size() > 3 ? static_cast<float>(std::atof(a[3].c_str())) : 300000.0f;
        auto e = world::level_instance::load(a[2], y);
        out += e.empty() ? std::format("loading {} at Y {:+.0f}\n", a[2], y) : "error: " + e + "\n";
    } else if (sub == "bg" && a.size() > 2 && a[1] == "boss") { // Dev: boss room check of the band at Y
        out += std::format("boss room at Y {}: {}\n", a[2], coop::maps::boss_room(static_cast<float>(std::atof(a[2].c_str()))));
    } else if (sub == "bg" && a.size() > 2 && a[1] == "unload") {
        world::level_instance::unload(a[2]);
        out += "unloading " + a[2] + "\n";
    } else if (sub == "bg") {
        out += world::level_instance::describe();
    } else if (sub == "trace" && a.size() > 1) {
        out += (coop::role() == coop::Role::Host ? coop::host::trace(a[1]) : coop::client::trace(a[1])) + "\n";
    } else if (sub == "cmdlog") { // Dev: cmdlog [owner filter] — read the starts since the last call, keep recording
        out += coop::commands::observe(true, a.size() > 1 ? a[1] : "") + "\n";
    } else if (sub == "status") {
        out += coop::describe();
        if (coop::role() == coop::Role::Host) out += coop::host::describe();
        if (coop::role() == coop::Role::Client) out += coop::client::describe();
    } else {
        throw std::runtime_error("usage: elmp lan host|join|leave|status");
    }
}

} // namespace

// elmp hunter start [kills N] [time S] [grace S] [respawn S] [target boss|<mapId>] [progress mirror|preset] | stop | status
void hunter_cmd(Args a, std::string& out) {
    const std::string sub = a.empty() ? "status" : a[0];
    if (sub == "start") {
        modes::hunter::Options o;
        for (std::size_t i = 1; i + 1 < a.size(); i += 2) {
            const auto& k = a[i];
            const auto& v = a[i + 1];
            if (k == "kills") o.kills = std::atoi(v.c_str());
            else if (k == "time") o.time_limit_s = std::atoi(v.c_str());
            else if (k == "grace") o.grace_s = std::atoi(v.c_str());
            else if (k == "respawn") o.respawn_s = std::atoi(v.c_str());
            else if (k == "target") o.target = v;
            else if (k == "progress") o.preset = v == "preset";
            else throw std::runtime_error("unknown option '" + k + "'");
        }
        const auto e = modes::hunter::start(o);
        out += e.empty() ? "hunter match started\n" : "error: " + e + "\n";
    } else if (sub == "stop") {
        modes::hunter::stop();
        out += "hunter match stopped\n";
    }
    out += modes::hunter::describe();
}

void register_coop_commands() {
    add("lan", "host <code> [port] [max] | join <addr> <code> [port] | leave | status | bg load|unload|boss", lan_cmd);
    add("sdk", "SDK mods (Mods/*/elmp.json): registration, slot ranges, subscriptions", [](Args, std::string& out) { out += sdk::describe(); });
    add("hunter", "start [kills N] [time S] [grace S] [respawn S] [target boss|<mapId>] [progress mirror|preset] | stop | status", hunter_cmd);
}

} // namespace elmp::console
