#pragma once

// Current values (not progress): HP, MP, remaining prayers (C7, FR-CORE-4a).
// Restore = the character's own controller `FullyRestore(Pawn)` (HP, prayers, skill uses) + MP through
// the vanilla component: `Add(max - curr)` when SpecialAttack (EAptitudeType 120) is unlocked, otherwise
// `Reset()` — the vanilla `Add` refuses MP without it and a direct CurrMP write would bypass that (v1
// core-facts「奥义解锁与 MP」). A dead pawn is not revived by HP (respawn does that). P1 is read-only.

#include <cstdint>
#include <string>

namespace elmp::core::vitals {

inline constexpr std::uint8_t kAptitudeSpecialAttack = 120;

struct Values {
    bool valid = false, alive = false;
    int hp = 0, max_hp = 0, mp = 0, max_mp = 0, heals = 0, max_heals = 0;
};

Values read(std::int64_t slot);        // works for slot 1 (read-only)
std::string restore(std::int64_t slot); // empty = ok; refuses slot 1 and non-ELMP controllers

} // namespace elmp::core::vitals
