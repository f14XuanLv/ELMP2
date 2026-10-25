#pragma once

// Hit rules (C4, FR-CORE-13 / §7.10): faction (relation to the world, vanilla `Faction`, 1 for every
// player) is separate from team (relation between players, ELMP data). This table holds slot → team and
// the pair rules (damage multiplier, knockback level); pvp/hitgate applies them inside vanilla hit
// windows. P1 (slot 1) is always friendly. Callers may only bind slots whose lease they hold.
// Game thread. `generation()` bumps on every change (hitgate rebuilds its controller cache from it).

#include "base/slots.h"

#include <cstdint>
#include <string>
#include <vector>

namespace elmp::core::hitrules {

enum class Team : std::uint8_t { Friendly = 1, Enemy = 2 };
const char* team_name(Team t) noexcept;

std::string bind(const slots::Owner& who, std::int64_t slot, Team team);
void unbind(std::int64_t slot);
void clear();

struct Binding {
    std::int64_t slot;
    Team team;
};
std::vector<Binding> bindings(); // includes P1 (friendly)
bool opposed();                  // at least one pair of opposing teams exists
std::uint64_t generation();

struct Rule {
    double damage = 0.1; // multiplier for hits between opposing players (0..1)
    int knockback = 0;   // 0 vanilla toughness, 1 light, 2 medium, 3 heavy (skip toughness check)
};
Rule rule();
void set_rule(const Rule& r);

void install(); // bindings are dropped with the world (seats / leases die with it)

} // namespace elmp::core::hitrules
