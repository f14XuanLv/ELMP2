#pragma once

// Skins (F1.12, FR-CORE-12): the six vanilla Lily looks p0001..p0006 (DT_SpineData_p0000 has 11 skin
// levels but six looks: atlas + skeleton p000N, notify always p0001, light MI_LilyLightN). The vanilla
// FindSpineDataForLevel reads P1's skin level. Pass the explicit level row to the original virtual
// SetSpineData instead: it owns asset replacement, animation refresh and the derived light material.
// DataTable rows are deep-copied through FProperty. P1 never goes through here.

#include "engine/ue.h"

#include <string>

namespace elmp::core::skin {

inline constexpr int kDefault = 1, kCount = 6;

constexpr bool valid(int n) { return n >= 1 && n <= kCount; }

std::string apply(ue::UObject* pawn, int n); // empty = ok
int current(ue::UObject* pawn);              // 0 = unknown

} // namespace elmp::core::skin
