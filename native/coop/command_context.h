#pragma once

#include "engine/handle.h"
#include <string>
#include <string_view>
#include <vector>

namespace elmp::coop::native_lily { struct Source; }

namespace elmp::coop::command_context {
bool enable();
void reset();
void forget(ue::UObject* pawn);
bool source(ue::UObject* pawn, native_lily::Source& out); // synchronous original command execution
bool retained_source(const native_lily::Source&);
std::vector<ue::UObject*> owned_states(ue::UObject* pawn);
struct Persistent {
    ue::UObject* cls = nullptr;
    std::uint64_t token = 0;
    std::uint32_t input = 0;
    std::uint8_t direction = 0, button = 0;
    float elapsed = 0;
};
Persistent persistent(ue::UObject* pawn);
std::uint64_t restore(ue::UObject* pawn, ue::UObject* cls, std::uint8_t direction, std::uint8_t button);
std::uint64_t play(ue::UObject* pawn, ue::UObject* cls, std::uint8_t direction, std::uint8_t button);
void finish(ue::UObject* pawn, std::uint64_t local_token, std::uint8_t reason);
void end_restored(ue::UObject* pawn, std::uint64_t token);
std::string fault();
std::string describe();
std::string trace(std::string_view action);
}
