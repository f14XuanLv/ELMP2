#pragma once

// Per-seat bindings (F1.3, FR-LOC-3a): action → keyboard virtual keys / gamepad controls. The device state
// is turned into actions, the actions into a virtual-pad frame (ECommandInputTypes bits + axes).
// Defaults equal the vanilla DefaultInput.ini (v1 gamepad_map, InputSettings.ActionMappings read
// 2026-10-06): keyboard arrows / Z jump / X attack / S, D skills / C dodge / V hook + interact / A heal /
// F switch set; pad A / X / Y / B / RT dodge / LT hook + interact / LB heal / RB switch, left stick or
// d-pad = direction. Two ELMP actions: equipment menu (Tab / Back) and HUD collapse (Q / R3).
// The PX equipment menu has fixed keys (F1.10, v1 FR-UI-5 2026-10-11) that bindings never change.

#include "base/json.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace elmp::input {

namespace gamepad {
struct State {
    std::uint16_t buttons = 0;
    std::uint8_t left_trigger = 0, right_trigger = 0;
    std::int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
};
inline constexpr std::int16_t kLeftThumbDeadzone = 7849; // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
inline constexpr std::uint8_t kTriggerThreshold = 30;    // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
} // namespace gamepad

namespace bindings {

enum Action : int { Up, Down, Left, Right, Jump, Attack, Summon1, Summon2, Dodge, Hook, Heal, SummonSwitch, Interact, Menu, HudToggle, Count };

std::string_view action_name(int action);
std::optional<int> action_named(std::string_view name);

// Pad controls: XInput button bits (0x0001..0x8000) plus trigger pseudo-bits.
inline constexpr std::uint32_t kLeftTrigger = 0x10000, kRightTrigger = 0x20000;

struct Bindings {
    std::array<std::vector<std::uint32_t>, Count> keys; // virtual-key codes
    std::array<std::vector<std::uint32_t>, Count> pad;  // pad controls
    bool operator==(const Bindings&) const = default;
};

const Bindings& defaults();
const Bindings& menu_keys(); // fixed equipment-menu keys; the first entry of each list is the hint label

json::Value to_json(const Bindings& b);
Bindings from_json(const json::Value& v, const Bindings& base); // missing actions keep base

struct Actions {
    std::uint32_t held = 0; // 1 << Action
    float x = 0, y = 0;     // y up = positive
    bool has(int a) const { return (held >> a) & 1u; }
};

Actions from_keyboard(const Bindings& b, const std::bitset<256>& keys);
Actions from_pad(const Bindings& b, const gamepad::State& s);
Actions merge(const Actions& a, const Actions& b);

// Actions → virtual-pad buttons (ECommandInputTypes bits).
std::uint32_t command_bits(const Actions& a);

std::string key_label(std::uint32_t vk);
std::string pad_label(std::uint32_t control);
std::optional<std::uint32_t> key_named(std::string_view label);  // "Z", "Tab", "VK65", "0x41"
std::optional<std::uint32_t> pad_named(std::string_view label);  // "A", "RT", "DPad Up", "Back"

} // namespace bindings
} // namespace elmp::input
