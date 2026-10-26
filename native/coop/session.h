#pragma once

// Coop session (C14 / §7.5): owns the net::Endpoint, the identity (FR-NET-8: protocol, EXE, main.dll,
// data content hashes) and dispatches inbound events on the game thread (frame-pump phase `coop`, only
// while a session exists — G0). Host and client modules subscribe through the handler table; messages are
// typed (coop/protocol). One session per process; role fixed for its lifetime.

#include "net/endpoint.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace elmp::coop {

enum class Role : std::uint8_t { None, Host, Client };

struct Handlers {
    void (*connected)(std::uint32_t peer) = nullptr;
    void (*closed)(std::uint32_t peer, const std::string& why, net::Reject reason) = nullptr;
    void (*reliable)(std::uint32_t peer, std::span<const std::uint8_t> msg) = nullptr;
    void (*unreliable)(std::uint32_t peer, std::uint8_t stream, std::span<const std::uint8_t> msg) = nullptr;
    void (*tick)(std::int64_t now_us) = nullptr;  // every frame while the session exists (after drain)
    void (*stop)() = nullptr;                     // session ended (leave / closed / teardown of the role)
};
void set_handlers(Role role, const Handlers& h);

std::string start_host(std::uint16_t port, std::string_view code, std::size_t max_peers);
std::string start_join(std::string_view addr, std::uint16_t port, std::string_view code);
void leave(std::string_view reason = "left");

Role role();
bool active();
void send_reliable(std::uint32_t peer, std::span<const std::uint8_t> msg); // peer 0 = all
void send_unreliable(std::uint32_t peer, std::uint8_t stream, std::span<const std::uint8_t> msg);
void kick(std::uint32_t peer, std::string_view reason);
net::EndpointStatus status();
std::string describe();
const std::string& last_error(); // last rejection / disconnect reason shown on the net page
net::Identity identity();        // computed once per process (cold)

void install(); // frame phase + teardown subscription

} // namespace elmp::coop
