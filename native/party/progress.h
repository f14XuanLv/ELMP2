#pragma once

// Shared progress (C6, F1.5; v1 party-facts「共享进度」「状态恢复与进度对齐修复验收」).
// Every second (frame-pump cadence, only with seats, never during a map transit):
//   1. snapshot each seat (direct reads: inventory Items rows, Currencies map, UnlockedAptitudes,
//      SpiritLevels, experience; a few getters) and compare with its baseline → the seat's own gains
//      (total experience across level-ups, three currencies, new rows in the aptitude / parameter /
//      passive / spirit inventories, spirit level increases);
//   2. grant them to P1 through vanilla entries (AddExperiencePoints(Δ, true), AddCurrency, AddItem,
//      SetSpiritLevel) — the only place ELMP writes P1 (X-4);
//   3. mirror P1 back to every seat whose signature differs (aptitudes and spirit levels exact, owned
//      items only added, level / prayer levels / relic slots only written when different, so prayers
//      are not refilled), then read back: success only when the seat matches P1; otherwise the reason
//      is kept per seat and retried next tick;
//   4. re-take the baseline after mirroring (no echo).
// HP / MP / remaining prayers / skill uses are not part of the signature.
// Join uses the same service: shared_profile() for the baseline, then sync().

#include "core/profile.h"

#include <cstdint>
#include <optional>
#include <string>

namespace elmp::party::progress {

std::optional<core::profile::Profile> shared_profile(std::string* err);

void joined(std::int64_t slot);
void forget(std::int64_t slot);
void forget_all();

std::string sync(std::int64_t slot); // immediate tick for one seat (respawn / menu); empty = aligned
void tick();                         // 1 s
// progress source "mirror" (hunters, F3.3): P1 is mirrored to the seat, its own gains are not granted to P1
void set_mirror_only(bool (*mirror_only)(std::int64_t slot));
std::string describe();

void install();

} // namespace elmp::party::progress
