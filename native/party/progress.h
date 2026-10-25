#pragma once

// Shared progress (C6, F1.5). P1b provides the join baseline: P1's shared fields (aptitudes, parameter
// items, spirit levels, owned relics and relic slots, level / experience / prayer levels) without P1's
// equipment (spirit sets, equipped relics stay per seat). The 1 s merge / mirror / read-back tick lands
// in P1c on top of the same per-seat records.

#include "core/profile.h"

#include <cstdint>
#include <optional>
#include <string>

namespace elmp::party::progress {

std::optional<core::profile::Profile> shared_profile(std::string* err);

void joined(std::int64_t slot); // seat baseline taken (P1c: start of its increment window)
void forget(std::int64_t slot);
void forget_all();

} // namespace elmp::party::progress
