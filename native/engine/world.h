#pragma once

// World facade: engine → viewport → world → game instance → P1 controller / pawn, and the
// WorldLoaderSubsystem. All reads are direct offset reads through resolved schemas (no ProcessEvent),
// except loader_flags() (two UFunction calls; used only while the world is settling) and the
// one-off saved_dir().

#include "engine/ue.h"

#include <string>

namespace RC::Unreal {
class UEngine;
}

namespace elmp::world {

void set_engine(RC::Unreal::UEngine* engine); // from the EngineTick callback

ue::UObject* engine();
ue::UObject* world();
ue::UObject* game_instance();
ue::UObject* p1_controller();
ue::UObject* p1_pawn();
ue::UObject* world_loader(); // cached handle; located by one object-array walk when missing (cold)

// UWorld::IsPaused without ProcessEvent: PersistentLevel.WorldSettings.PauserPlayerState != null (the vanilla
// SetGamePaused path; the pause delay and editor / client cases do not apply to this game).
bool paused();

ue::FName current_map_id();  // WorldLoaderSubsystem.CurrentGameMapID (direct read); None if unknown
bool loader_flags(bool& loading, bool& ready); // IsLoading(true) / IsGameMapReady(true) via ProcessEvent

std::wstring saved_dir(); // KismetSystemLibrary::GetProjectSavedDirectory (once, cached)

// Locate a live, non-default instance of `cls` (whole object array walk; counted as reflect.scans).
ue::UObject* find_instance(ue::UClass* cls);

std::string schema_status(); // empty = all resolved
void retry_schema();         // allow one more resolution attempt after a failure (lifecycle transitions)

} // namespace elmp::world
