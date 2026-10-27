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
    // 5 Act, 10 Fx, 11 FxStop, 12 Snd, 13 ActStop: P2 / P6 product channels, retired by ADR 0006 (protocol 4)
    Build = 6,      // client → host: guest build json (guestKey + equipment)
    EquipRequest = 7, // client → host: menu action json
    EquipResult = 8,  // host → client: result json
    Control = 9,    // either: code + text
    Cmd = 14,       // host → client: CommandQueue command start of a replicated actor (ADR 0006)
    SeatBuild = 15, // host → client: json {seat, build} — a seat's equipment for its client copy
};

// Owner of a replicated command: which presented actor on the client it belongs to.
enum class RefKind : std::uint8_t {
    Enemy = 1,  // id = spawn point name id (section e key)
    Dyn = 2,    // id = dyn id (section ed key)
    Lily = 3,   // id = seat key (section l key; 1 = host P1)
    Spirit = 4, // host-side classification only (spirit commands derive from the summoner, never sent)
};
inline constexpr std::uint8_t kRefKindMax = 4;

struct Welcome { std::uint8_t seat = 0; std::uint16_t epoch = 0; std::uint16_t map = 0; };
struct NameDef { std::uint16_t id = 0; std::string text; };
struct MapMsg { std::uint16_t map = 0; std::string tag; };
struct MapReady { std::uint16_t map = 0; };
// ADR 0006 decision replication: the host's command start, replayed on the client through the vanilla
// AddNewImmediateCommand(dir, cmd, class). Combat / movement command types only (ECommandInputTypes 0..8;
// INTERACT / MAINMENU / PAUSE / MINIMAP never travel).
inline constexpr std::uint8_t kCmdTypeMax = 8, kCmdDirMax = 31;
struct Cmd {
    std::uint8_t kind = 1;   // RefKind
    std::uint16_t net_id = 0;
    std::uint32_t seq = 0;   // per owner, monotonic
    std::uint32_t t_ms = 0;  // host clock at the start (snapshot time base)
    std::uint16_t cls = 0;   // CommandAction class path name id
    std::uint8_t dir = 0;    // EDirectionInputTypes
    std::uint8_t cmd = 0;    // ECommandInputTypes
};
struct Json { std::string text; };
struct Control { std::uint8_t code = 0; std::string text; };
inline constexpr std::uint8_t kControlBanner = 10; // host → client: mode status line (empty = clear)

std::size_t encode(const Welcome& m, std::span<std::uint8_t> out);
std::size_t encode(const NameDef& m, std::span<std::uint8_t> out);
std::size_t encode(const MapMsg& m, std::span<std::uint8_t> out);
std::size_t encode(const MapReady& m, std::span<std::uint8_t> out);
std::size_t encode(Msg type, const Json& m, std::span<std::uint8_t> out); // Build / EquipRequest / EquipResult
std::size_t encode(const Control& m, std::span<std::uint8_t> out);
std::size_t encode(const Cmd& m, std::span<std::uint8_t> out);
bool decode(std::span<const std::uint8_t> in, Cmd& m);

Msg type_of(std::span<const std::uint8_t> in); // 0 cast when empty
bool decode(std::span<const std::uint8_t> in, Welcome& m);
bool decode(std::span<const std::uint8_t> in, NameDef& m);
bool decode(std::span<const std::uint8_t> in, MapMsg& m);
bool decode(std::span<const std::uint8_t> in, MapReady& m);
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
