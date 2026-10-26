#pragma once

// Death / respawn / recall / placement (C9, F1.8; v1 party/respawn + party-facts「召回」):
//   - a seat that died (CurrHP 0) or lost its pawn while its controller lives is revived after 1.5 s next
//     to P1 — progress aligned first, then pawn-only respawn (equipment stays on the controller);
//   - recall: more than recallFallDepth below P1 → at once ("fell"); farther than recallDistance for
//     recallDelayMs → "far"; not while P1 is dead, a transit runs or the seat's menu is open;
//   - placement shared by join / revive / recall / transit: behind P1, 70 UU × n (cycle 4), +10 Z;
//     teleports clear velocity.
// Patrol cadence 250 ms (also drives map rule 2 and the bench check).

#include "core/player.h"

#include <cstdint>
#include <string>

namespace elmp::party::respawn {

inline constexpr std::int64_t kDeathDelayMs = 1500;

std::string teleport(std::int64_t slot, const core::Vec3& at);
void freeze(std::int64_t slot);  // stop + MOVE_None (no gravity while the old area unloads)
void release(std::int64_t slot); // MOVE_None → MOVE_Falling
std::string revive(std::int64_t slot, const core::Vec3& at); // progress sync + pawn-only respawn
std::string place_at(std::int64_t slot, const core::Vec3& at, bool frozen, bool* revived = nullptr);
bool alive(std::int64_t slot);

void patrol(std::int64_t now_us); // 250 ms
// a mode that owns death handling / recall of some seats (hunter); nullptr = none
void set_override(bool (*owns)(std::int64_t slot));
void forget(std::int64_t slot);
std::string describe();

void install();

} // namespace elmp::party::respawn
