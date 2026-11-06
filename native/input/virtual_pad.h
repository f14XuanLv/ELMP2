#pragma once

// Virtual pad per slot (M5): detour of the snapshot consume `InputConsume` (EL.json, prefix-checked);
// after the vanilla consume copies Next → Current, the pad's frame is written into the Current snapshot
// of that pawn's ZenithInputComponent. Pawn input stays disabled (spawn), so the pad is the only source.
// Entries are keyed by component pointer and validated by handle (index + serial) on every match;
// a dead or re-possessed component drops its entry (core.pad_dropped). Game thread only.
// Slot 1 (P1) can be driven only in Dev builds after an explicit allow (F5.3).

#include "input/pad_state.h"

#include "engine/ue.h"

#include <cstdint>
#include <string>

namespace elmp::input::pad {

std::string attach(std::int64_t slot, ue::UObject* pawn, ue::UObject* controller);
void detach(std::int64_t slot); // zeroes the snapshot if the component is still alive
void forget_all();              // world teardown: drop entries without touching UObjects

std::string set(std::int64_t slot, double x, double y, std::int64_t raw);
std::string axes(std::int64_t slot, double x, double y);
std::string hold(std::int64_t slot, std::uint32_t bits, bool down);
std::string tap(std::int64_t slot, std::uint32_t bits);

struct Info {
    bool attached = false;
    float x = 0, y = 0;
    std::uint32_t raw = 0, presses = 0;
    std::uint64_t frames = 0;
};
Info info(std::int64_t slot);
std::uint32_t take_presses(std::int64_t slot); // press edges since the last call (bench / menu edges)
// Reliable remote simulation supplies one exact consumed snapshot, never regenerated edges.
std::string snapshot(std::int64_t slot, const InputSnapshot& value);
using ConsumeObserver = void (*)(ue::UObject* pawn, const InputSnapshot& value);
bool observe(ConsumeObserver observer); // same native detour; no second hook on InputConsume

std::string allow_player1(bool on);
bool player1_allowed();
std::size_t attached();

} // namespace elmp::input::pad
