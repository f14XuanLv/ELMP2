#pragma once

// Spawn / remove / respawn the N-th Lily (M3, FR-CORE-1). Vanilla pipeline requirements (v1 core-facts
// 「生成第 N 玩家」, ELMP 2026-10-06/11):
//   - the pawn must be possessed by a ZenithPlayerController with a PlayerState (PC_Base_C);
//   - spawning a PC mid-game BeginPlays PlayerCameraManagerZenith with a null PCOwner and crashes →
//     the PC class CDO's PlayerCameraManagerClass is swapped to the engine base class for the spawn;
//   - controllers cannot be destroyed (K2_DestroyActor is a no-op) → pooled per slot; a reused PC keeps
//     the previous progress, so callers reset the profile after spawning;
//   - BP_p0000_Lily_C has no Spine data → skin::apply; Yaw must be ±90 (Spine is a billboard);
//   - remove: UnequipAllSpirits → UnPossess → destroy the pawn (never the spirits by hand);
//   - respawn = vanilla RespawnPlayerPawn semantics (IDA 0x140D232D0): UnPossess + destroy the pawn only,
//     then ReequipSpirits on the new pawn; equipment / inventory stay on the controller.
// The caller holds the slot lease. Game thread, world Settled.

#include "core/player.h"

#include <optional>
#include <string>

namespace elmp::core::spawn {

struct Options {
    std::optional<Vec3> location; // default: next to P1 (+120 Y, +10 Z)
    float yaw = 90.0f;
    int skin = 1;
};

std::string spawn(std::int64_t slot, const Options& opt);  // empty = ok; rolls back on failure
std::string despawn(std::int64_t slot, bool keep_equipment); // false keep = leave (unequip spirits)
std::string respawn(std::int64_t slot, const Options& opt); // pawn only + ReequipSpirits + restore

} // namespace elmp::core::spawn
