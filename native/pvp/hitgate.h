#pragma once

// PvP hit windows (M8, C4). Vanilla gates every hit on `attacker.IsPlayerFaction() != target.IsPlayerFaction()`
// (party-facts「PvP 阵营与伤害门」), and every player stays Faction 1. For an attacker / target pair of
// opposing ELMP teams (core/hitrules) the target's FactionComponent.Faction is written to 2 (Enemies) for
// the duration of one vanilla call and restored right after — damage, toughness, knockback, death all stay
// in the vanilla pipeline. Inside the same window the attacker's DamagePercentBonus gets (scale-1)*100 and,
// at knockback levels 1..3, vanilla-active AE_Knockback components skip the toughness check with profile
// data. Windows:
//   - UFunction pre/post on AbilityHitbox:OnActorOverlapBegin / ProcessActorEnter and
//     Ability:ExecuteAbilityEffectsOnTarget (synchronous effects),
//   - manifest detour AbilityEffectQueue (queues resumed by AAbility::Tick after HitStop, e.g. POST_DEFAULT
//     AE_Knockback).
// Installed on the first opposing pair only (G0: nothing hooked without PvP); afterwards a flag test.
// Pair resolution walks Owner → Instigator → Pawn.Controller and compares controller handles: no lookups.

#include <cstdint>
#include <string>

namespace elmp::pvp {

void install(); // frame phase: keeps the controller cache in step with core/hitrules
void uninstall();

struct Status {
    bool hooked = false, active = false;
    std::string failure;
    std::uint64_t calls = 0, flips = 0, scales = 0, knocks = 0, effects = 0;
};
Status status();
std::string describe();

} // namespace elmp::pvp
