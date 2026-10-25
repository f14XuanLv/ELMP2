#pragma once

// Frame pump (§3, M22): the only per-frame ELMP entry on the game thread is the UEngine::Tick
// pre-callback. Phases run in registration order; each has an enable predicate (evaluated every tick, so
// it must be a couple of loads) and an optional cadence. With no seats and no session only the
// always-on phases (lifecycle; 1 Hz registry audit) run — G0.
//
// Metrics per tick: frame.elmp_us (ELMP time inside the tick callback), frame.<phase>_us, and
// frame.interval_us (tick-to-tick, informative). "Idle" ticks (Settled world, no seats, no session)
// additionally count reflect.calls issued during the tick into g0.reflect_calls (gate: 0).

#include <cstdint>

namespace elmp::frame {

struct Tick {
    std::int64_t now_us;
    float dt;
};

using PhaseFn = void (*)(const Tick&);
using Predicate = bool (*)();

void add_phase(const char* name, PhaseFn fn, Predicate enabled = nullptr, std::int64_t period_ms = 0);

// Activity predicates provided by higher layers (seats / session); default: none active.
void set_activity(Predicate seats, Predicate session);
bool idle(); // settled world, no seats, no session

void install();
void uninstall();

std::uint64_t ticks();

} // namespace elmp::frame
