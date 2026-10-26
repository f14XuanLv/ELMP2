#pragma once

// Typed coop messages besides the snapshot (§8.2 / §8.3). Pure data + codec (CTest-able).
// Unreliable streams: kSnapStream (host → client snapshot), kInputStream (client → host input).
// Reliable: first byte = Msg type; strings are bounded; every decoder rejects trailing / short input.

#include "net/codec.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace elmp::net::proto {

inline constexpr std::uint8_t kSnapStream = 1, kInputStream = 2;
inline constexpr std::size_t kMaxName = 200, kMaxJson = 16 * 1024;

// Upstream input (v1 net-facts「会话与呈现」: axes + held bits + per-command 16-bit press counters + epoch).
struct Input {
    std::uint16_t epoch = 0;
    std::int8_t x = 0, y = 0;     // -100..100
    std::uint16_t raw = 0;        // held command bits (1 << ECommandInputTypes)
    std::array<std::uint16_t, 10> presses{}; // cumulative press count per command 1..9 (index 0 unused)
    std::uint32_t snap_ack = 0;   // latest snapshot seq applied by the client
};
std::size_t encode(const Input& in, std::span<std::uint8_t> out);
bool decode(std::span<const std::uint8_t> in, Input& out);

enum class Msg : std::uint8_t {
    Welcome = 1,    // host → client: seat, epoch, host map name id
    NameDef = 2,    // host → client: id ↔ utf8
    Map = 3,        // host → client: follow to map (name id) via player start tag
    MapReady = 4,   // client → host: arrived in map
    Act = 5,        // host → client: enemy ability launch replay
    Build = 6,      // client → host: guest build json (guestKey + equipment)
    EquipRequest = 7, // client → host: menu action json
    EquipResult = 8,  // host → client: result json
    Control = 9,    // either: code + text
};

struct Welcome { std::uint8_t seat = 0; std::uint16_t epoch = 0; std::uint16_t map = 0; };
struct NameDef { std::uint16_t id = 0; std::string text; };
struct MapMsg { std::uint16_t map = 0; std::string tag; };
struct MapReady { std::uint16_t map = 0; };
struct Act {
    std::uint16_t net_id = 0; // enemy row key (e) or dyn id (ed when dyn = 1)
    std::uint8_t dyn = 0;
    std::uint32_t seq = 0;
    std::uint16_t ability = 0; // class path name id
    std::array<float, 10> xf{0, 0, 0, 1, 0, 0, 0, 1, 1, 1};  // FAbilityDescription.Offset: quat (4), translation (3), scale (3)
    std::array<float, 10> xf2{0, 0, 0, 1, 0, 0, 0, 1, 1, 1}; // the call's Offset argument
    std::uint8_t spawn_bone = 0, attach_bone = 0;             // ESpineBone values
    std::uint16_t custom_spawn = 0, custom_attach = 0;        // name ids (0 = None)
    std::uint8_t flags = 0;     // bit0 follow spawn rot, bit1 follow attach rot, bit2 fire-and-forget
};
struct Json { std::string text; };
struct Control { std::uint8_t code = 0; std::string text; };

std::size_t encode(const Welcome& m, std::span<std::uint8_t> out);
std::size_t encode(const NameDef& m, std::span<std::uint8_t> out);
std::size_t encode(const MapMsg& m, std::span<std::uint8_t> out);
std::size_t encode(const MapReady& m, std::span<std::uint8_t> out);
std::size_t encode(const Act& m, std::span<std::uint8_t> out);
std::size_t encode(Msg type, const Json& m, std::span<std::uint8_t> out); // Build / EquipRequest / EquipResult
std::size_t encode(const Control& m, std::span<std::uint8_t> out);

Msg type_of(std::span<const std::uint8_t> in); // 0 cast when empty
bool decode(std::span<const std::uint8_t> in, Welcome& m);
bool decode(std::span<const std::uint8_t> in, NameDef& m);
bool decode(std::span<const std::uint8_t> in, MapMsg& m);
bool decode(std::span<const std::uint8_t> in, MapReady& m);
bool decode(std::span<const std::uint8_t> in, Act& m);
bool decode(std::span<const std::uint8_t> in, Json& m);
bool decode(std::span<const std::uint8_t> in, Control& m);

// Session name table: host interns strings to ids (≥ 1); NameDefs go out before the first use.
class NameTable {
public:
    std::uint16_t intern(std::string_view s, bool* is_new = nullptr); // 0 when full
    const std::string* text(std::uint16_t id) const;
    void define(std::uint16_t id, std::string_view s); // client side
    void clear();
    std::size_t size() const { return names_.size(); }
    const std::vector<std::string>& all() const { return names_; } // index = id - 1 (late join replay)

private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, std::uint16_t> ids_;
};

} // namespace elmp::net::proto
