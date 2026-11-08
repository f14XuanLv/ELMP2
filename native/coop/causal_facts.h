#pragma once
#include "coop/causality.h"
#include "coop/ability.h"
#include "coop/native_lily.h"
namespace elmp::coop::causality { bool retained_source(const native_lily::Source& source); }
namespace elmp::coop::causality::facts {
bool install();
void clear();
void launch(ue::UObject*, ue::UObject*, const ability::Identity&, native_lily::Source);
void prepare_reuse(ue::UObject* actor);
ue::UObject* query(const ability::Identity&, ue::UObject*, float, float, bool, std::uint8_t, bool, ue::UObject*);
ue::UObject* target(ue::UObject* component, ue::UObject* selected);
bool homing(ue::UObject* component, const ability::Identity&, float); // false: terminal/invalid context, do not tick
void stopping(const ability::Identity&, std::uint8_t);
void end(const ability::Identity&);
std::string describe();
}
