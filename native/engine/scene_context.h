#pragma once
#include "engine/handle.h"
#include <array>
#include <string>
namespace elmp::scene_context {
struct Context {ObjHandle actor,root,aim;};
// Numeric presentation context, never a gameplay entity. World owns the Actor until destroy().
bool create(Context& value,bool separate_aim=false);
bool pose(Context&,const std::array<float,3>& position,const std::array<float,3>& rotation,const std::array<float,3>& scale,const std::array<float,3>& velocity);
bool aim(Context&,const std::array<float,3>& position,const std::array<float,3>& velocity);
void destroy(Context&);
std::string describe();
}
