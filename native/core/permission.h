#pragma once

// Permission gates (C5, M9): per seat and per class. A closed gate = every primitive component of the
// seat's pawn ignores the target actors when moving (`PrimitiveComponent:IgnoreActorWhenMoving`); UE 4.26
// `UpdateOverlapsImpl` adds MoveIgnoreActors to the sweep params, so no overlap starts and the vanilla
// door / bench never sees the seat (v1 party-facts「切图门」, engine PrimitiveComponent.cpp).
// Defaults for non-P1 seats (§3 C5): travel off, rest off, trigger.story off, trigger.world on,
// pickup on, interact on, save off. P1 is not managed — except the explicit map-rule-2 door hold.
//
// GateCache (§16.1): per slot {pawn, primitive components, target → ignored}. Invalidation: pawn change
// (respawn), flag change, registry bucket version (new door / bench streamed in), teardown. Audit: the
// 500 ms tick compares versions; `elmp gates` lists applied counts.
// Targets (registry buckets): travel = BP_WorldTravelVolume_C, rest = BP_Interactable_RestingPoint_C,
// interact = AInteractable, pickup = ACollectable, trigger.story / trigger.world = ATrigger by class
// (data/gates.json; unlisted = story, reported once). An object in several buckets (a bench is also an
// Interactable) is ignored when any of its gates is closed. `save`: PX have no vanilla save entry — the
// only one reachable by a body is the bench (EVT_Rest, closed by `rest`); the flag is recorded, not a target.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace elmp::core::permission {

enum class Gate : std::uint8_t { Travel, Rest, TriggerStory, TriggerWorld, Pickup, Interact, Save, Count };
const char* gate_name(Gate g) noexcept;
bool gate_named(std::string_view name, Gate& out);

void set(std::int64_t slot, Gate g, bool allowed); // slot >= 2
bool allows(std::int64_t slot, Gate g);              // effective (incl. the grant hook)

// Rule 1 grants travel to local seats (§7.13.4): the party installs a grant predicate.
using Grant = bool (*)(std::int64_t slot, Gate g);
void set_grant(Grant grant);

// Map rule 2: P1 ignores every door except `open_door` (registry handle index; 0 = none open).
void hold_p1_doors(bool hold, std::int32_t open_door_index = 0);

void track(std::int64_t slot);  // a seat to manage (join / respawn)
void forget(std::int64_t slot); // seat left (revokes nothing: the pawn is gone or leaving)
void tick();                    // 500 ms: re-apply on version / pawn / flag changes
std::string describe();

std::string load(const std::filesystem::path& gates_json); // defaults + trigger class policy; empty = ok
void install(); // teardown subscription

} // namespace elmp::core::permission
