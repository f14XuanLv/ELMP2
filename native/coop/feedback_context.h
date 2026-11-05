#pragma once

#include "engine/ue.h"

#include <string>

namespace elmp::coop::feedback_context {

void install(); // initialization only: registers the inactive low-frequency frame phase
bool enable(); // session-only observer + exact script pre/post hooks; idempotent
void disable();
bool scoped();
bool effect_scope(); // current exact source is an Effect graph (Client refuses that entire graph)
ue::UObject* sound_target(ue::UObject* context);
bool owns(ue::UObject* source);
std::string fault();
std::string describe();

} // namespace elmp::coop::feedback_context
