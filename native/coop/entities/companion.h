#pragma once

#include "engine/handle.h"

#include <cstdint>

namespace elmp::coop { class IEntityAdapter; }
namespace elmp::coop::entities {

// True only for this Companion's exact Blueprint FX property, never by particle asset/class name.
// SpawnEmitterAttached runs before Blueprint assignment: callers must also test at event delivery.
bool companion_owns_fx(ue::UObject* owner, ue::UObject* psc);
// Map reset retires rows but preserves the allocator; session end is its only reset source.

} // namespace elmp::coop::entities
