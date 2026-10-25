#pragma once

// ELMP manager, game thread side (F1.17, FR-UI-2a): executes overlay Requests (open / close with the world
// paused, add / remove player, skin, side, device assignment, mute, key-binding capture, rules page →
// settings) and publishes overlay::Manager.
// Button visibility (every frame, direct reads only so the no-seat idle frame stays at 0 reflection calls):
// world settled, P1 has a pawn, world not paused, no map transit, no PX menu, manager closed; with seats also
// no vanilla navigable menu on screen (MenuOpenCache). Without seats a non-pausing vanilla menu (the bench
// rest menu) is not detected and the button stays visible (needs ProcessEvent; G0, decision 2026-10-26).
// PX storage (P1e) shows "arrives with P1e" until save_scope exists.

#include <string>

namespace elmp::party::manager {

std::string open();
void close();
bool is_open();
std::string describe();
void install();

} // namespace elmp::party::manager
