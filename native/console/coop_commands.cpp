// Coop console commands (P2): `elmp lan host|join|leave|status`. The manager's net page calls the same
// functions (F2.1).

#include "console/console.h"

#include "coop/session.h"

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
    } else if (sub == "status") {
        out += coop::describe();
    } else {
        throw std::runtime_error("usage: elmp lan host|join|leave|status");
    }
}

} // namespace

void register_coop_commands() { add("lan", "host <code> [port] [max] | join <addr> <code> [port] | leave | status", lan_cmd); }

} // namespace elmp::console
