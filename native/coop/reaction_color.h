#pragma once

#include "coop/reaction.h"

namespace elmp::coop::reaction::color {

bool host_enable(void (*sink)(const Capture&), bool (*filter)(ue::UObject*));
void host_disable();
bool client_enable(bool on);
bool replay(ue::UObject* actor, const net::proto::Reaction& event, std::string* why);
std::string host_fault();
void reset();
std::string describe();

} // namespace elmp::coop::reaction::color
