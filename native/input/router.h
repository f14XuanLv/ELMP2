#pragma once

// Input routing (C3, F1.2/F1.4): device → seat assignment, per-keyboard key state, ownership of vanilla
// keyboard messages, per-seat merge of devices, P1 gamepad merge, mute. Win32 / XInput wiring lives in
// rawinput.cpp / xinput.cpp; this file is the routing logic.
//
// Rules (v1 FR-LOC-3, unchanged):
//   - device ids: keyboard "kbd:<vid_xxxx&pid_xxxx | lower-case Raw Input path>", pad "xinput:<0..3>";
//   - effective owner: assigned to a joined local seat → that seat; unassigned or the seat is absent → P1;
//   - a vanilla WM_KEY* message is matched with the latest Raw Input event of the same key / edge to find
//     its keyboard; if that keyboard's owner is not P1 the message is not delivered (unmatched → deliver);
//   - what the game reads as pad 0 = merge of every connected pad owned by P1 (buttons OR, sticks / triggers
//     strongest); pads owned by seats or merged into 0 report "connected, no input";
//   - mute: every physical keyboard / pad is silent for everyone; the assignment table is kept.
// G0: while `active()` is false (no device assigned to a joined seat, not muted) the wiring passes
// everything through untouched — the game behaves exactly as without ELMP.

#include "input/bindings.h"

#include <atomic>
#include <bitset>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace elmp::input::router {

inline constexpr std::int64_t kP1 = 1;
inline constexpr int kPads = 4;

std::string pad_id(int index);
std::optional<int> pad_index(std::string_view id);

using PadReader = std::optional<gamepad::State> (*)(int index);

// Assignment table (persisted by party/devices).
void assign(const std::string& device, std::int64_t slot); // slot <= 1 → back to P1 (entry removed)
std::map<std::string, std::int64_t> assignments();
void set_assignments(std::map<std::string, std::int64_t> table);
void set_seats(const std::set<std::int64_t>& seats); // joined local seats driven by devices
std::int64_t owner(const std::string& device);
bool has_devices(std::int64_t slot);
bool has_pad(std::int64_t slot);

void set_muted(bool muted);
bool muted();
bool active(); // lock-free; see G0 above
std::uint64_t swallowed();

// Keyboards (window thread).
void raw_key(const std::string& keyboard, std::uint32_t vk, bool down);
bool legacy_allowed(std::uint32_t vk, bool down);
void clear_keys();
bool any_key(const std::string& keyboard); // manager device indicator

// Bindings.
void set_bindings(std::int64_t slot, bindings::Bindings b);
bindings::Bindings bindings_of(std::int64_t slot);
std::map<std::int64_t, bindings::Bindings> all_bindings();

// Seat actions from every device assigned to it; `fixed` replaces the seat bindings (menu keys).
bindings::Actions seat_actions(std::int64_t slot, PadReader read, const bindings::Bindings* fixed = nullptr);
std::uint32_t pressed(std::int64_t slot, const bindings::Actions& now); // new action bits since the last call

// What the game reads as pad 0 (nullopt: no connected pad owned by P1).
std::optional<gamepad::State> player1_pad(PadReader read);
void set_connected(int index, bool connected); // refreshed at low frequency by the pump
bool connected(int index);

} // namespace elmp::input::router
