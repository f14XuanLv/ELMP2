#pragma once

// Map-follow (C8, F1.7; v1 party-facts「过图时序」2026-10-08). Every vanilla SwitchGameMap (door, P1 death
// respawn, fast travel) runs PC_Base_C:OnSwitchGameMapStart → OnGameMapSwitch → OnGameMapReadyPostFade
// for every controller; only Controller0's drive this machine:
//   start   → freeze carried seats (pad zeroed, stop, MOVE_None: no fall while the old area unloads);
//   switch  → once P1 stands at the new entrance (new pawn / moved > 50 UU / had no pawn), place the seats
//             behind P1 inside the black screen (dead seats are revived there), still frozen;
//   fade-in (WBP_FaderLoading_C:LaunchFadeIn) → release (MOVE_Falling), they land during the fade;
//   ready   (PostFade, or switched + loader ready) → finish; 20 s timeout forces place + release.
// Hooks: one UE4SS ProcessLocalScriptFunction post-callback comparing Stack.Node() with the four resolved
// UFunction pointers (registered when the first seat joins; blueprint functions resolved once loaded —
// UE4SS's own UFunction hook path compares GetFullName() strings on every script call, too expensive).
// While a transit runs the patrol, recall and progress stay out (they skip on active()).

#include <cstdint>
#include <string>

namespace elmp::party::transit {

bool active();
// coop: seats standing in another map (host background instance) when P1 switches are not carried
void set_stay(bool (*stay)(std::int64_t slot));
bool hooked(); // the three PC events are resolved and hooked
void arm();    // seats exist: register the callback (idempotent) and resolve functions
std::string describe();
void install();

} // namespace elmp::party::transit
