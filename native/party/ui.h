#pragma once

// Game-thread services shared by the HUD feed, the PX menu and the manager.
//   - pause holders: the vanilla GameplayStatics::SetGamePaused(world, true) when the first holder appears and
//     the world was running; resumed when the last holder leaves, only if ELMP paused it (never un-pauses a
//     vanilla menu). Client holders block own input while its Host replay keeps running.
//     Dropped with the world (the new world starts unpaused).
//   - MenuOpenCache: any vanilla navigable menu (registry Menu bucket = UserWidgetNavigable, created / deleted
//     via BeginPlay-free object listeners) with IsInViewport && IsVisible; refreshed at most every 250 ms;
//     invalidated by bucket insertions and teardown. ProcessEvent per widget: only used while seats exist.
//   - DisplayPlayerUI (GameSettingsSubsystem, vanilla "show HUD" option): same 250 ms cache.
//   - head-anchor projection: GameplayStatics::ProjectWorldToScreen(P1 controller, …) — 1 call per seat per frame.

#include "core/player.h"

#include <optional>
#include <string>

namespace elmp::party::ui {

enum Holder : unsigned { Menu = 1, Manager = 2 };
void hold_pause(Holder who, bool on);
unsigned pause_holders();
bool enable_client();
void disable_client();
bool gameplay_blocked(); // Client's own gameplay input only; remote playback keeps running
std::string client_status();

bool vanilla_menu_open();
bool display_player_ui();

struct Screen {
    float x, y;
};
// Direct camera-cache math, audited 1 Hz against vanilla ProjectWorldToScreen (falls back on mismatch).
std::optional<Screen> project(const core::Vec3& world);
std::string projection_status();

float special_cost(); // GameLogicBPFLibrary::GetSpecialAttackMPCost (once per world)

void install();

} // namespace elmp::party::ui
