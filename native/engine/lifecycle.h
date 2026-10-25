#pragma once

// World lifecycle (M13): Boot → Title | Loading → Settling → Settled → TearingDown → Loading …
//   - TearingDown: UEngine::LoadMap pre-callback (the old world is still intact) → teardown subscribers
//     run (save data, drop UObject references), then the registry is cleared.
//   - Settling → Settled: polled at 250 ms while not settled: world + WorldLoaderSubsystem present,
//     CurrentGameMapID set, P1 pawn alive, IsGameMapReady(true) and !IsLoading(true). Save identity
//     (save_scope, P1 phase) will join this predicate.
//   - Settled: step() does nothing per frame. SwitchGameMap (streaming) does not go through LoadMap and
//     does not leave Settled; map transitions are tracked by the transit hooks (P1).

#include <cstdint>
#include <string>

namespace elmp::lifecycle {

enum class State : std::uint8_t { Boot, Title, Loading, Settling, Settled, TearingDown };
const char* name(State s) noexcept;

using Callback = void (*)();
void on_teardown(Callback cb);
void on_settled(Callback cb);

void install();
void uninstall();
void step(std::int64_t now_us); // frame pump, every tick

State state();
std::uint64_t generation();      // incremented on every Settled
const std::string& waiting_for(); // why not settled yet (diagnostics)
std::int64_t settled_at_us();
std::uint64_t teardowns();

} // namespace elmp::lifecycle
