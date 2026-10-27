#pragma once

// Coop host role: remote seats, input, per-peer snapshot deltas, map follow (see host.cpp).

#include <string>

namespace elmp::coop { struct HostCtx; }

namespace elmp::coop::host {

// adapter halves (coop/entities/{lily,enemy}.cpp)
void write_lilies(HostCtx& c);
void write_enemies(HostCtx& c);
void write_spirits(HostCtx& c);
void reset_spirits();

void install(); // registers the Host handlers with coop/session
// mode status line to the peer seated at only_seat (0 = every peer); shown as the client's banner
void broadcast_banner(const std::string& text, std::int64_t only_seat = 0);
std::string describe();
std::string sync_report(const std::string& cat); // `elmp lan sync` JSON lines (coop/sync.h)
std::string rec(const std::string& sub, int frames); // `elmp lan rec start [frames] | dump`
std::string trace(const std::string& spawn_point); // Dev: previous trace of captured Y, starts a new one

} // namespace elmp::coop::host
