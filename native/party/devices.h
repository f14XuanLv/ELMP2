#pragma once

// Device assignment, per-seat bindings and the per-frame input pump (C3, F1.2–F1.4).
//   - Saved/ELMP/ELMP/devices.json (schema 1): {assignments: {device: slot}, bindings: {slot: {keys, pad}}};
//     machine-wide (not per save, v1 FR-LOC-11), atomic write, loaded once the saved dir is known;
//   - wiring (window subclass + Raw Input, XInput import entry) is installed the first time it is needed
//     (a device seat joins or input is muted) and passes everything through while the router is inactive;
//   - pump (frame phase, only while a device seat exists): seat actions → virtual pad; 1 Hz pad probe;
//     press edges of the seat's Menu / HUD-collapse actions are kept for the overlay (P1d).

#include <cstdint>
#include <set>
#include <string>

namespace elmp::party::devices {

std::string load(); // empty = ok (missing file = defaults)
std::string save();

std::string ensure_wiring();
void sync_seats(const std::set<std::int64_t>& device_seats);
std::string auto_assign(std::int64_t slot, int preferred_pad); // -1 = first free connected pad

void pump(std::int64_t now_us); // frame phase
bool pumping();

std::uint32_t take_ui_presses(std::int64_t slot); // Menu / HudToggle press edges (bindings::Action bits)

void install(); // frame phase + settings listener

} // namespace elmp::party::devices
