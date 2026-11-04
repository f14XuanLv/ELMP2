#pragma once

#include "engine/ue.h"
#include "net/snapshot.h"

#include <string>
#include <string_view>

namespace elmp::coop::presentation {

using NameIntern = std::uint16_t (*)(std::string_view);
using NameLookup = const std::string* (*)(std::uint16_t);
// Actual Spine current tracks, pause and effective track rates, not Zenith logical animation layers.
// False leaves a validity-zero pose. Apply checks actual local phase even when Host phase is unchanged.
bool capture(ue::UObject* actor, NameIntern intern, net::snap::Pose& out);
bool apply(ue::UObject* actor, const net::snap::Pose& pose, NameLookup lookup);
// Invalidation sources: actor/spine ObjHandle expiry, native state recreation, skin replacement,
// map/session reset, and a 60 s cache expiry. No UObject/native pointer is used without a fresh owner.
void forget(ue::UObject* actor);
void reset();
std::string describe();

} // namespace elmp::coop::presentation
