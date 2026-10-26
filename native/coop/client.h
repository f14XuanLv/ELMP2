#pragma once

// Coop client role (§7.5 client/*): Build → Welcome, local input upstream, snapshot decode, enemy takeover
// (guard marks + frozen tick) and presentation, map follow.

#include <string>

namespace elmp::coop::client {

void install(); // registers the Client handlers with coop/session
std::string describe();
std::string trace(const std::string& spawn_point); // Dev: returns the previous trace, starts a new one (240 frames of presented Y)

} // namespace elmp::coop::client
