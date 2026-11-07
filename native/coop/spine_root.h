#pragma once
#include "engine/ue.h"
#include <array>
#include <string>
namespace elmp::coop::spine_root {
bool capture(ue::UObject* actor,std::array<float,2>& scale);
bool apply(ue::UObject* actor,const std::array<float,2>& scale);
void forget(ue::UObject* actor);
void reset();
std::string describe();
}
