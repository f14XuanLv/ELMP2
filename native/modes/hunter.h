#pragma once

// Hunter mode (G3 / F3.1–F3.4, FR-HUNT-7, C19). One speedrunner = host P1 (friendly, no added latency)
// against every other seat (local PX or remote guests, at most 4) as hunters (enemy team, PvP on).
//   grace   : headStartSec (15) — hunters frozen where they stand;
//   running : monotonic clock; runner wins on reaching the target map or when a boss spawn point of the
//             foreground map becomes cleared (target "boss"); hunters win with hunterTargetKills runner
//             deaths (1) or when timeLimitSec (> 0) runs out;
//   ended   : winner + reason kept until `elmp hunter stop`.
// Hunter seats: travel gate on (remote ones switch only their own instance, coop/maps), progress source
// "mirror" (P1 mirrored, own gains not written back) or "preset" (FULL profile), death owned by the mode:
// respawn after hunterRespawnDelaySec (8) at the entrance (PlayerStart) of the map they are in, full state.
// Benches only refill the hunter (rest gate stays closed). Compass: same map → direction + distance to the
// runner, other map → the runner's map id (HUD prompt for local seats, banner + Control message for guests).

#include <cstdint>
#include <string>
#include <vector>

namespace elmp::modes::hunter {

struct Options {
    int kills = 1;          // hunterTargetKills
    int time_limit_s = 0;   // timeLimitSec, 0 = none
    int grace_s = 15;       // headStartSec
    int respawn_s = 8;      // hunterRespawnDelaySec
    std::string target = "boss"; // "boss" or a map id
    bool preset = false;    // progress source: false = mirror, true = preset (FULL)
};

std::string start(const Options& o); // empty = started
void stop();
bool active();                       // grace or running
bool is_hunter(std::int64_t slot);
std::string compass(std::int64_t slot); // HUD prompt line for a local hunter seat ("" = none)
void tick(std::int64_t now_us);      // frame phase (only while a match exists)
std::string describe();
void install();

} // namespace elmp::modes::hunter
