#pragma once

// Coop client role (§7.5 client/*): Build → Welcome, local input upstream, snapshot decode, enemy takeover
// (guard marks + frozen tick) and presentation, map follow.

#include <cstdint>
#include <string>

namespace elmp::coop { struct ClientCtx; }

namespace elmp::coop::client {

void install(); // registers the Client handlers with coop/session
std::string describe();
std::string sync_report(const std::string& cat); // `elmp lan sync` JSON lines (coop/sync.h)
std::string rec(const std::string& sub, int frames);
void set_ev_mask(std::uint32_t mask); // Dev bisect: skip event types on the client (1 Act 2 Fx 4 FxStop 8 Snd 16 ActStop) // `elmp lan rec start [frames] | dump`
std::string trace(const std::string& spawn_point);
// adapter enemy_dyn (client half): adopt / spawn, takeover, present the ed rows; removal of vanished rows
void present_dynamic(const ClientCtx& c);
// adapter halves enemy / lily (coop/entities/{enemy,lily}.cpp)
void present_enemies(const ClientCtx& c);
void present_lily_rows(const ClientCtx& c);
void present_spirits(const ClientCtx& c); // adapter spirit (client half)
void reset_spirits();
std::string describe_dynamic(); // Dev: returns the previous trace, starts a new one (240 frames of presented Y)

} // namespace elmp::coop::client
