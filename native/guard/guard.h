#pragma once

// Client-side settlement interception / save protection / activation gates (C15, M18, Host-authority
// rule: the guest process never settles). Native detours from data/binary/EL.json, installed on first
// arm and pass-through while disarmed (one flag load). Game thread only.
//   - DoDamage / skill damage: blocked unless the target is inside a replay window (the client replays
//     host-confirmed hits through the vanilla pipeline);
//   - IsPlayer thunk: false for marked actors (presented copies / the hidden input anchor);
//   - SaveGame / SaveGameAsync: refused (client save is read-only for the whole session);
//   - AZenithAIController::Activate / AZenithCharacter::Activate: skipped for marked enemies (no AI
//     decision on the client, engine-facts「敌人激活入口重新定位」);
//   - SetActorEnableCollision / SetActorTickEnabled: the enabling direction is skipped for marked actors;
//   - hit effects (ADR 0006): UAbilityEffectComponent OnApplyEffect / OnFinalizeEffect run only for the
//     presentation classes (AE_HitStop / CameraShake / ShakeTarget / ForceFeedback); every other effect
//     class (damage, knockback, stun, heal, MP, stamina, unknown) is not applied (fail closed);
//   - UDeathComponent::StartDeathProcess: skipped for marked actors (death is snapshot presentation).

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
// ADR 0006: decision sources (AI controllers) never get tick / collision back; execution actors (presented
// pawns whose vanilla execution layer runs) keep ticking — the disabling direction is refused. Other marked
// actors keep the P2 rule (enabling refused).
void mark_decision(ue::UObject* actor, bool on = true);
void mark_exec(ue::UObject* actor, bool on = true);

// Replay window: damage to this actor (its HP component's owner) is allowed until close.
void open_replay(ue::UObject* target);
void close_replay(ue::UObject* target);

struct Counters {
    std::uint64_t damage_blocked, damage_replayed, skill_blocked, skill_replayed, isplayer_false, save_blocked, ai_activate_blocked, char_activate_blocked,
        collision_blocked, tick_blocked, effect_blocked, effect_passed, death_blocked, tick_kept;
};
Counters counters();
std::string describe();

} // namespace elmp::guard
