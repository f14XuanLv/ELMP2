#pragma once

// Client presentation copies of host-spawned actors (enemy_dyn, pickup): vanilla deferred spawn
// (GameplayStatics BeginDeferredActorSpawnFromClass + FinishSpawningActor) at the host position; the caller
// freezes / marks the copy. Class lookup by path is cached (StaticLoadObject costs milliseconds).

#include "engine/ue.h"

#include <string>

namespace elmp::coop::entities {

ue::UClass* class_by_path(const std::string& path); // nullptr when it does not load
ue::UObject* spawn_copy(ue::UClass* cls, float x, float y, float z);
void destroy_copy(ue::UObject* actor);
bool move_copy(ue::UObject* actor, float x, float y, float z); // K2_SetActorLocation (no sweep)

} // namespace elmp::coop::entities
