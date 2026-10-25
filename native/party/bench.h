#pragma once

// Benches for PX (C11, F1.9; v1 party-facts「椅子」): the vanilla rest (EVT_Rest) restores Controller0,
// saves the game and respawns enemies, so the `rest` gate stays closed for PX (their collision ignores
// BP_Interactable_RestingPoint_C and the vanilla interaction never starts). A PX near a bench
// (|ΔY| ≤ 150, |ΔZ| ≤ 250) pressing interact (accumulated press edge, short taps are not lost) gets the
// vanilla FullyRestore of its own controller (HP / prayers / skill uses) — no save, no map change.
// Cooldown 3 s on the monotonic clock, bound to the current pawn (a new pawn does not inherit it).
// Saving the PX equipment on rest arrives with the cold storage (P1e).

#include <cstdint>
#include <string>

namespace elmp::party::bench {

void poll(std::int64_t slot);       // patrol: consumes interact presses
std::string rest(std::int64_t slot); // empty = rested; otherwise the refusal reason
void forget(std::int64_t slot);
void install();

} // namespace elmp::party::bench
