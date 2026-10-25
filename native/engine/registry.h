#pragma once

// Object registry (§5, M17): the watched objects of the current world, bucketed by kind and kept current
// by events instead of scans:
//   - AActor BeginPlay (post) → classify (cached per class) → insert;
//   - AActor EndPlay (pre) and the UObject delete listener → remove;
//   - UObject create listener → only for non-actor kinds (Menu), only while a consumer wants them;
//   - one object-array walk when the world settles (baseline), none at run time;
//   - 1 Hz audit: every entry's handle is re-validated (memory reads only); stale entries are counted
//     (registry.audit.stale — expected 0) and removed.
// `cross_check()` walks the object array on demand and diffs it with the registry (P0 gate).

#include "engine/handle.h"
#include "engine/ue.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace elmp::registry {

enum class Kind : std::uint8_t {
    Enemy,
    SpawnPoint,
    Lily,
    PlayerController,
    Interactable,
    RestingPoint,
    Breakable,
    Collectable,
    Trigger,
    TravelVolume,
    ActivationVolume,
    Menu, // non-actor (UserWidgetNavigable); maintained only while wanted
    Count
};
inline constexpr std::size_t kKinds = static_cast<std::size_t>(Kind::Count);
const char* kind_name(Kind k) noexcept;

struct Entry {
    ObjHandle handle;
    ue::FName name;
    std::uint16_t class_id = 0; // index into class_names()
    std::uint16_t map_slot = 0; // 0 = foreground map (background maps: P3)
    std::uint16_t net_id = 0;   // assigned by coop (P2)
    std::uint16_t flags = 0;
};

void install();   // hooks + listeners; call once after UE4SS is initialised
void uninstall();

void baseline();  // world settled: one object-array walk merged into the buckets
void clear();     // world teardown
void audit();     // 1 Hz from the frame pump
void drain();     // per frame while Menu is wanted: moves created widgets into the bucket

void want_menu(bool on);
bool menu_wanted();

std::span<const Entry> bucket(Kind k);
// Incremented on every insert into the bucket (consumers re-apply per-object state when it changes).
std::uint64_t version(Kind k);
std::size_t size(Kind k);
std::string class_name(std::uint16_t class_id);

struct Stats {
    std::array<std::uint32_t, kKinds> count{};
    std::uint64_t inserted = 0, removed_endplay = 0, removed_deleted = 0, audit_stale = 0, audits = 0;
    std::uint64_t baseline_added = 0, baselines = 0;
    double baseline_ms = 0;
};
Stats stats();

struct VerifyResult {
    std::array<std::uint32_t, kKinds> scanned{}, registered{}, missing{}, extra{};
    std::vector<std::string> samples; // up to 16 "kind: name (missing|extra)" lines
    double ms = 0;
    bool consistent() const;
};
VerifyResult cross_check();

} // namespace elmp::registry
