#pragma once

// HUD feed (F1.7, §7.4 hud_feed, §16.1 SkillCache): fills overlay::Hud on the game thread while seats exist.
//   every frame: head anchor (1 ProjectWorldToScreen per seat), the cached skill actions' remaining value /
//                cooldown (only actions with a cast limit / recast time; ProcessEvent);
//   every 250 ms: HP / MP / prayers, special gauge unlock, current summon set, the SkillCache key (pawn +
//                summon set + the three equipped CommandActionType classes, read directly from SummonSets),
//                key labels, bench prompt.
// SkillCache = per seat the three CommandAction objects of the current set (vanilla
// CommandQueueComponent::GetOrCreateCommandAction(SpiritData.CommandSettingsData.CommandSettings.CommandActionType),
// exactly what WBP_SummonItem binds) with their limit type and recast time. Invalidation: key change (equip /
// unequip / set switch / new pawn), teardown. Audit: hud.skill_rebuilds counter + `elmp hud`.
// Hidden while: map transit, world paused, PX menu / manager open, a vanilla navigable menu on screen, or the
// vanilla "display HUD" option off. The seat's Menu / HUD-collapse presses are consumed here.

#include <cstdint>
#include <string>

namespace elmp::party::hud_feed {

void toast(std::int64_t slot, std::string_view text, double seconds = 2.5);
void forget(std::int64_t slot);
std::string describe();
void install();

} // namespace elmp::party::hud_feed
