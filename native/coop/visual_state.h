#pragma once

#include "engine/ue.h"
#include "net/visual.h"
#include <string>

namespace elmp::coop::visual_state {
void capture(ue::UObject* actor, net::snap::Visual& out);
bool apply(ue::UObject* actor, const net::snap::Visual& in);
void forget(ue::UObject* actor);
void reset();
std::string describe();
} // namespace elmp::coop::visual_state
