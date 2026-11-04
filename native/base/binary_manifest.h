#pragma once

// Runtime model of data/binary/EL.json (M1): native function RVAs + byte prefixes, and raw struct offsets
// that have no reflection entry. Parsing and prefix comparison are pure; the game-side checks live in
// preflight (prefixes) and engine/reflect (offset checks against reflected neighbours).

#include "base/json.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace elmp::binary {

struct Function {
    std::string name;
    std::uintptr_t rva = 0;
    std::vector<std::uint8_t> prefix;
    std::string signature, owner, source;
    std::string reflect; // "/Script/Pkg.Class:Func" when a UFunction exists; its native address is preferred
};

struct OffsetCheck {
    enum class Kind : std::uint8_t { Equals, Within, StructSize, NativeOffset } kind = Kind::Equals;
    std::string cls;       // Equals / Within: owning class; StructSize: struct path
    std::string property;  // Equals
    std::string after;     // Within: the reflected field this offset follows
    std::string before;    // Within: the reflected field it precedes, or "<end>" = class PropertiesSize
    std::string function; // NativeOffset: verified function whose prefix contains the displacement
    std::uint32_t byte = 0, width = 0;
};

struct Offset {
    std::string name;
    std::int64_t value = 0;
    std::int32_t size = 0; // 0 = unspecified
    OffsetCheck check;
    std::string source;
};

struct Manifest {
    std::string module;
    std::map<std::string, Function, std::less<>> functions;
    std::map<std::string, Offset, std::less<>> offsets;

    const Function* fn(std::string_view name) const {
        auto it = functions.find(name);
        return it == functions.end() ? nullptr : &it->second;
    }
    const Offset* offset(std::string_view name) const {
        auto it = offsets.find(name);
        return it == offsets.end() ? nullptr : &it->second;
    }
};

inline constexpr std::size_t kMinPrefix = 6;

std::optional<std::vector<std::uint8_t>> parse_hex(std::string_view hex);
std::optional<Manifest> parse(std::string_view text, std::string* err);

// Compares the bytes at `code` with the expected prefix. `code` must already be readable.
bool prefix_matches(std::span<const std::uint8_t> code, std::span<const std::uint8_t> prefix) noexcept;

} // namespace elmp::binary
