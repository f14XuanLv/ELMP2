#pragma once

#include "engine/handle.h"

#include <cstdint>

namespace elmp::coop { class IEntityAdapter; }
namespace elmp::coop::entities {

IEntityAdapter& companion();
// Host: only successfully published rows resolve to event owners. IDs never wrap within a session.
std::uint16_t companion_id_of(const ObjHandle& actor);
std::uint8_t companion_seat_of(std::uint16_t id);
// Client: a ready copy, including a hidden one (an FX stop still needs its owner).
ue::UObject* companion_actor_of(std::uint16_t id);
// True only for this Companion's exact Blueprint FX property, never by particle asset/class name.
// SpawnEmitterAttached runs before Blueprint assignment: callers must also test at event delivery.
bool companion_owns_fx(ue::UObject* owner, ue::UObject* psc);
// Map reset retires rows but preserves the allocator; session end is its only reset source.
void companion_end_session();

} // namespace elmp::coop::entities
