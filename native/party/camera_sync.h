#pragma once

// Shared camera (F1.6, C10): local seat controllers are registered as vanilla ally targets of P1's
// AGameplayCamera (RegisterAllyController), so the vanilla multi-target framing includes them; framing
// margins / MaxDistance scale go to camera/framing while at least one local seat exists.
// Cache: camera handle (P1 camera manager → RegisteredGameplayCamera) + registered controllers;
// invalidated when the camera object changes (map switch / reload) and on world teardown.
// Audit: every 500 ms the ally array is read back directly and missing seats are re-registered
// (camera.register_calls counts every ProcessEvent).
// Client sessions instead sanitize all four target arrays every frame, keeping only vanilla P1.
// Audit: elmp camera reports all target counts and camera.client_targets_removed; no cached target data.

#include <cstdint>
#include <string>

namespace elmp::party::camera_sync {

void install();
// Seat left (called after it is removed from the seat table): unregister its controller now, and switch
// framing off with the last seat, so no camera work is left for idle frames (G0).
void left(std::int64_t slot);
std::string describe();

} // namespace elmp::party::camera_sync
