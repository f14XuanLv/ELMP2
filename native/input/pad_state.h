#pragma once

// Virtual pad state (M5): held buttons (raw), taps that are never lost even if shorter than a frame,
// and press edges (flags). Bit i = 1 << ECommandInputTypes (JUMP=1 … INTERACT=9) → mask 0x3FE.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace elmp::input {

// UZenithInputComponent snapshot layout (40 bytes; NextInputSnapshot +184, InputSnapshot +224 —
// data/binary/EL.json, checked against reflection when the world settles).
struct InputSnapshot {
    float input_x = 0, input_y = 0;
    float right_x = 0, right_y = 0;
    float move_x = 0, move_y = 0, move_z = 0;
    std::uint8_t direction = 0, padding[3]{};
    std::uint32_t flags = 0, raw = 0;
};
static_assert(sizeof(InputSnapshot) == 40);

inline constexpr std::uint32_t kCommandMask = 0x3FE;

// ECommandInputTypes (D:\CodeT\EnderLilies\Source\Zenith\Public\ECommandInputTypes.h)
enum class Command : std::uint8_t { None = 0, Jump, Dodge, Attack, Summon1, Summon2, SummonSetSwitch, Hook, Heal, Interact };
std::optional<Command> command_named(std::string_view name); // "JUMP", "jump", "3" …
const char* command_name(Command c) noexcept;
std::string bits_text(std::uint32_t bits);

// EDirectionInputTypes from the axes (dead zone 0.5).
std::uint8_t direction(float x, float y) noexcept;

class PadState {
public:
    std::string set(double x, double y, std::int64_t raw); // error text, state unchanged on error
    std::string set_axes(double x, double y);
    std::string hold(std::uint32_t bits, bool down);
    std::string tap(std::uint32_t bits); // press edge on the next consumed frame
    void clear() { *this = PadState{}; }

    InputSnapshot consume(); // once per consumed frame: flags = taps | newly held bits

    float x() const { return x_; }
    float y() const { return y_; }
    std::uint32_t raw() const { return raw_; }

private:
    float x_ = 0, y_ = 0;
    std::uint32_t raw_ = 0, previous_raw_ = 0, pending_ = 0;
};

} // namespace elmp::input
