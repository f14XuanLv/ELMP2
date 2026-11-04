#pragma once

// ADR 0008: fixed policy for host-owned presentation actors. Game thread only.
// apply() is idempotent and retains the first RootMotionFactor for each component.
// A failed apply leaves logic ticks disabled; the caller must not present that actor.
// release/reset stop skeleton ticks before restoring saved factors; they never resume actor ticks.

#include "engine/ue.h"

#include <string>

namespace elmp::coop::presentation_policy {

bool apply(ue::UObject* actor);
void release(ue::UObject* actor);
void reset();

// Pure battle-colour component entry; never invokes Pawn.OnChangeTarget.
// An actor without BattleColorComponent is a successful no-op.
bool engage(ue::UObject* actor, bool on);
std::string describe();

} // namespace elmp::coop::presentation_policy
