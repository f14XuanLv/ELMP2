#include "input/pad_state.h"

#include <array>
#include <cctype>
#include <cmath>

namespace elmp::input {

namespace {

constexpr std::array<const char*, 10> kNames{"NONE", "JUMP", "DODGE", "ATTACK", "SUMMON_1", "SUMMON_2", "SUMMON_SET_SWITCH", "HOOK", "HEAL", "INTERACT"};

bool axis_ok(double v) { return std::isfinite(v) && std::abs(v) <= 1.0; }

} // namespace

std::optional<Command> command_named(std::string_view name) {
    if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
        const int v = std::atoi(std::string(name).c_str());
        if (v >= 1 && v <= 9) return static_cast<Command>(v);
        return std::nullopt;
    }
    for (std::size_t i = 1; i < kNames.size(); ++i) {
        const std::string_view n = kNames[i];
        if (n.size() != name.size()) continue;
        bool eq = true;
        for (std::size_t k = 0; k < n.size() && eq; ++k) eq = std::toupper(static_cast<unsigned char>(name[k])) == n[k];
        if (eq) return static_cast<Command>(i);
    }
    return std::nullopt;
}

const char* command_name(Command c) noexcept {
    const auto i = static_cast<std::size_t>(c);
    return i < kNames.size() ? kNames[i] : "?";
}

std::string bits_text(std::uint32_t bits) {
    std::string out;
    for (std::size_t i = 1; i < kNames.size(); ++i) {
        if (!(bits & (1u << i))) continue;
        if (!out.empty()) out += '|';
        out += kNames[i];
    }
    return out.empty() ? "-" : out;
}

std::uint8_t direction(float x, float y) noexcept {
    const bool right = x > 0.5f, left = x < -0.5f, up = y > 0.5f, down = y < -0.5f;
    if (up && right) return 5;
    if (up && left) return 6;
    if (down && right) return 7;
    if (down && left) return 8;
    if (up) return 1;
    if (down) return 2;
    if (left) return 3;
    return right ? 4 : 0;
}

std::string PadState::set(double x, double y, std::int64_t raw) {
    if (!axis_ok(x) || !axis_ok(y)) return "axis must be finite and within [-1, 1]";
    if (raw < 0 || (static_cast<std::uint64_t>(raw) & ~static_cast<std::uint64_t>(kCommandMask))) return "buttons outside 0x3FE";
    x_ = static_cast<float>(x);
    y_ = static_cast<float>(y);
    raw_ = static_cast<std::uint32_t>(raw);
    return {};
}

std::string PadState::set_axes(double x, double y) { return set(x, y, raw_); }

std::string PadState::hold(std::uint32_t bits, bool down) {
    if (!bits || (bits & ~kCommandMask)) return "buttons outside 0x3FE";
    raw_ = down ? (raw_ | bits) : (raw_ & ~bits);
    return {};
}

std::string PadState::tap(std::uint32_t bits) {
    if (!bits || (bits & ~kCommandMask)) return "tap bits must be non-empty and within 0x3FE";
    pending_ |= bits;
    return {};
}

InputSnapshot PadState::consume() {
    InputSnapshot s;
    // Same fields the vanilla consume fills from a stick: input vector, right vector, move input (Y/Z plane).
    s.input_x = s.right_x = s.move_y = x_;
    s.input_y = s.right_y = s.move_z = y_;
    s.direction = direction(x_, y_);
    s.raw = raw_;
    s.flags = pending_ | (raw_ & ~previous_raw_);
    previous_raw_ = raw_;
    pending_ = 0;
    return s;
}

} // namespace elmp::input
