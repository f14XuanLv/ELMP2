#pragma once

// Coop client role (§7.5 client/*): Build → Welcome, local input upstream, snapshot decode, enemy takeover
// (ADR 0006: decision source stopped, vanilla execution runs host commands) and presentation, map follow.

#include <string>

namespace elmp::coop { struct ClientCtx; }

namespace elmp::coop::client {

void install(); // registers the Client handlers with coop/session
std::string describe();
std::string trace(const std::string& spawn_point);
// adapter enemy_dyn (client half): adopt / spawn, takeover, present the ed rows; removal of vanished rows
void present_dynamic(const ClientCtx& c);
// adapter halves enemy / lily (coop/entities/{enemy,lily}.cpp)
void present_enemies(const ClientCtx& c);
void present_lily_rows(const ClientCtx& c);
std::string describe_dynamic(); // Dev: returns the previous trace, starts a new one (240 frames of presented Y)

} // namespace elmp::coop::client
