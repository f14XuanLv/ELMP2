#pragma once

// Coop host role: remote seats, input, per-peer snapshot deltas, map follow (see host.cpp).

#include <string>

namespace elmp::coop { struct HostCtx; }

namespace elmp::coop::host {

// adapter halves (coop/entities/{lily,enemy}.cpp)
void write_lilies(HostCtx& c);
void write_enemies(HostCtx& c);

void install(); // registers the Host handlers with coop/session
std::string describe();
std::string trace(const std::string& spawn_point); // Dev: previous trace of captured Y, starts a new one

} // namespace elmp::coop::host
