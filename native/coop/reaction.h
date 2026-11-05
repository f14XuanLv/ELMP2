#pragma once

#include "engine/ue.h"
#include "net/protocol.h"

#include <string>
#include <vector>

namespace elmp::coop::reaction {

struct Capture {
    ue::UObject* owner = nullptr;
    std::uint64_t entry_order = 0; // native entry order; Host sorts post-call captures before assigning wire seq
    net::proto::Reaction event; // host routing fills seat/epoch/map/seq; t_ms is the native call time
    std::string curve;
    std::vector<std::string> states; // host flush interns assets after the native call has returned
};
using NameLookup = const std::string* (*)(std::uint16_t);
std::uint64_t next_capture_order(); // process lifetime; shared by native reactions, 0 means exhausted
bool host_enable(void (*sink)(const Capture&), bool (*filter)(ue::UObject*) = nullptr);
void host_disable();
std::string host_fault(); // accepted Host reaction could not be captured; caller ends session on next tick
bool client_enable(bool on); // only the local controlled Lily's spontaneous Launch is refused
bool replay(ue::UObject* actor, const net::proto::Reaction& event, NameLookup lookup, std::string* why);
bool active(ue::UObject* local_pawn); // original Knockback/recovery or HitStop; unresolved enabled context returns true
void reset(); // releases cached schemas; does not uninstall the native detour
std::string describe();

} // namespace elmp::coop::reaction
