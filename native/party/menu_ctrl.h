#pragma once

// PX equipment menu (F1.10; v1 FR-UI-5, party-facts「原版装备菜单」), game thread side:
//   - opened by the seat's Menu action (or `elmp menu <slot>`): shared progress is synced first (owned spirits /
//     relics and spirit levels come from P1), the world is paused (party/ui pause holder);
//   - navigation from the seat's devices with the fixed menu keys (pad A confirm / B, Back return / X unequip /
//     LB, RB tabs; keyboard Z, Enter / Esc, D, Tab / X / A, F), direction auto-repeat; script seats via console;
//   - changes go through the vanilla entries: spirits = profile apply (SpiritEquipComponent Unequip / Equip,
//     any spirit in any cell like WBP_EquipmentMenu:EquipItemToSlot, a spirit already in the set moves);
//     relics = PassiveEquipComponent EquipPassive (vanilla refusals: slots / limit / invalid) / UnequipPassive;
//   - only at a bench unless equipAnywhere (vanilla ERROR_ONLY_RESTPOINT);
//   - closed by Back / the Menu action, the seat leaving or dying, a map transit, or world teardown.
// The model (overlay::Menu) is republished on every change.

#include <cstdint>
#include <string>
#include <string_view>

namespace elmp::party::menu_ctrl {

std::string open(std::int64_t slot); // empty = opened
void close();
std::int64_t open_slot(); // 0 = closed
std::string input(std::string_view what); // up/down/left/right/confirm/back/context/prev/next (script seats, tests)
std::string describe();
void install();

} // namespace elmp::party::menu_ctrl
