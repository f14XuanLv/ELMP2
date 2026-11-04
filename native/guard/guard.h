#pragma once

// Client-side settlement interception / save protection / activation gates (C15, M18, Host-authority
// rule: the guest process never settles). Native detours from data/binary/EL.json, installed on first
// arm and pass-through while disarmed (one flag load). Game thread only.
//   - DoDamage / skill damage: blocked unless the target is inside a replay window (the client replays
//     host-confirmed hits through the vanilla pipeline);
//   - Effect Apply / Finalize event thunks and DeathStart: always refused while armed, including inside
//     a damage replay window. These are verified standard paths, not proof that every bypass is closed;
//   - IsPlayer thunk: false for marked actors (presented copies / the hidden input anchor);
//   - SaveGame / SaveGameAsync: refused (client save is read-only for the whole session);
//   - AZenithAIController::Activate: refused throughout a client session; Character::Activate is
//     refused for marked actors (engine-facts「敌人激活入口重新定位」);
//   - SetActorEnableCollision / SetActorTickEnabled: the enabling direction is skipped for marked actors.

#include "engine/ue.h"

#include <cstdint>
#include <string>

namespace elmp::guard {

bool arm(); // installs on first use; false (and logs) if a detour could not be installed
void disarm();
bool armed();

void mark(ue::UObject* actor);   // presented enemy / controller / volume / copy
void unmark(ue::UObject* actor);
void unmark_all();
bool marked(ue::UObject* actor);
void mark_not_player(ue::UObject* actor, bool on); // IsPlayer → false (anchor, copies)
// Replay window: damage to this actor (its HP component's owner) is allowed until close.
void open_replay(ue::UObject* target);
void close_replay(ue::UObject* target);

struct Counters {
    std::uint64_t damage_blocked, damage_replayed, skill_blocked, skill_replayed, isplayer_false, save_blocked, ai_activate_blocked, char_activate_blocked,
        collision_blocked, tick_blocked, effect_apply_blocked, effect_finalize_blocked, death_start_blocked;
};
Counters counters();
std::string describe();

} // namespace elmp::guard
