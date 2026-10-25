#pragma once

// Slot → controller / pawn of this world (FR-CORE-5, M12). The core owns the table: every ELMP
// controller it spawned is kept as a handle (pooled per slot, FR-SLOT-2) and tagged on the actor
// (`ELMP`, `ELMP.Slot.N`) so other tools and the registry can tell ELMP actors apart. Lookups are
// handle reads (index + serial) — no scans. Slot 1 is the original player, read-only.
//
// Invalidation: the table is dropped on world teardown (controllers die with the world); a pooled
// controller that disappeared is removed on the next lookup (counted: core.pool_lost).

#include "engine/handle.h"
#include "engine/ue.h"

#include "base/slots.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace elmp::core {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

slots::Table& leases();
slots::Owner owner_for(std::int64_t slot, std::string_view name = "ELMP"); // Elmp for 2..100, DevTool for 101..1000

ue::UObject* controller(std::int64_t slot); // slot 1 = GameplayStatics player 0 (P1)
ue::UObject* pawn(std::int64_t slot);       // possessed pawn whose Controller points back, else nullptr
bool is_elmp_controller(const ue::UObject* pc);
std::vector<std::int64_t> slots_in_world(); // ELMP slots with a pooled controller
std::size_t active();                       // ELMP slots with a possessed pawn (frame-pump activity predicate)

// Identity tags (cold path).
void tag(ue::UObject* actor, std::int64_t slot);
std::int64_t slot_of(ue::UObject* actor); // 0 = not an ELMP actor

// Pool bookkeeping for spawn.
void remember(std::int64_t slot, ue::UObject* pc);
void set_pawn(std::int64_t slot, ue::UObject* pawn);

std::optional<Vec3> location(ue::UObject* actor); // root component RelativeLocation (direct read)

struct State {
    bool valid = false, alive = false, busy = false;
    Vec3 pos;
    int hp = 0, max_hp = 0, mp = 0, max_mp = 0, heals = 0, max_heals = 0;
    std::string animation;
    float track_time = 0;
};
State state(std::int64_t slot); // cold path (ProcessEvent for animation / heals / busy)

void install(); // lifecycle subscriptions
std::string describe();

} // namespace elmp::core
