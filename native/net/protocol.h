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
    Fx = 10,        // host → client: SpawnFX on a replicated actor's FX component
    FxStop = 11,    // host → client: DeactivateSystem of a replicated SpawnFX result
    Snd = 12,       // host → client: sound timeline event on a replicated actor
    ActStop = 13,   // host → client: AAbility::Stop of a replicated launch
    // 14 was the removed experimental Cmd channel; never reuse its wire id.
};

// Owner of a replicated event (P6): which presented actor on the client it belongs to.
enum class RefKind : std::uint8_t {
    Enemy = 1,  // id = spawn point name id (section e key)
    Dyn = 2,    // id = dyn id (section ed key)
    Lily = 3,   // id = seat key (section l key; 1 = host P1)
    Spirit = 4, // id = spirit row key (section sp: seat << 8 | index)
    Companion = 5, // id = independent companion row key (section cp; seat is explicit)
};
inline constexpr std::uint8_t kRefKindMax = 5;

struct Welcome { std::uint8_t seat = 0; std::uint16_t epoch = 0; std::uint16_t map = 0; };
struct NameDef { std::uint16_t id = 0; std::string text; };
struct MapMsg { std::uint16_t map = 0; std::string tag; };
struct MapReady { std::uint16_t map = 0; };
struct Act {
    std::uint16_t net_id = 0; // owner id (meaning per kind)
    std::uint8_t kind = 1;    // RefKind
    std::uint32_t seq = 0;
    std::uint16_t ability = 0; // class path name id
    std::array<float, 10> xf{0, 0, 0, 1, 0, 0, 0, 1, 1, 1};  // FAbilityDescription.Offset: quat (4), translation (3), scale (3)
    std::array<float, 10> xf2{0, 0, 0, 1, 0, 0, 0, 1, 1, 1}; // the call's Offset argument
    std::uint8_t spawn_bone = 0, attach_bone = 0;             // ESpineBone values
    std::uint16_t custom_spawn = 0, custom_attach = 0;        // name ids (0 = None)
    std::uint8_t flags = 0;     // bit0 follow spawn rot, bit1 follow attach rot, bit2 fire-and-forget
};
enum class FxAttach : std::uint8_t { None = 0, Root = 1, Component = 2, CompanionFx = 3, SpineBone = 4 };
inline constexpr bool valid_attach_bone(std::uint8_t bone) {
    switch (bone) {
    case 10: case 50: case 60: case 70: case 71: case 72: case 100: case 110: case 120: case 130:
    case 180: case 181: case 182: case 200: case 210: case 230: case 231: case 232: return true;
    default: return false; // None uses explicit Root; custom names have no proven enum mapping.
    }
}
struct Fx {
    std::uint8_t kind = 1;  // RefKind of the FX component's owner
    std::uint16_t net_id = 0;
    std::uint32_t seq = 0;  // host-wide FX sequence (FxStop target)
    std::uint16_t ps = 0;   // UParticleSystem path name id
    std::uint8_t bone = 0;  // ESpineBone
    std::uint16_t custom_bone = 0; // name id (0 = None)
    std::array<float, 10> xf{0, 0, 0, 1, 0, 0, 0, 1, 1, 1}; // Offset
    std::uint8_t flags = 0; // bit0 attach, bit1 follow bone rotation, bit2 running baseline (BaseFX register / active emitter), bit3 emitter (xf = relative rotator/loc/scale), bit4 initial state
    std::int32_t sort = 0;  // BaseFX TranslucencySortPriority; emitter original EAttachLocation (audit)
    std::uint8_t pool = 0;  // Host EPSCPoolMethod (audit); remote replay always uses None
    std::uint8_t attach_kind = 0; // FxAttach; emitter only, no implicit root fallback
    std::uint16_t attach_component = 0; // owner-relative component path; Component/SpineBone only
    std::uint16_t attach_socket = 0; // exact AttachPointName; 0 = None
    std::uint8_t emitter_flags = 0; // bit0 original AutoDestroy, bit1 original AutoActivate
};
struct FxStop { std::uint32_t seq = 0; };struct Snd {
    std::uint8_t kind = 1;
    std::uint16_t net_id = 0;
    std::uint16_t event = 0; // UFMODEvent path name id
    std::uint8_t is2d = 0;
};
struct ActStop {
    std::uint8_t kind = 1;
    std::uint16_t net_id = 0;
    std::uint32_t seq = 0; // the Act.seq of that owner
    std::uint8_t reason = 0; // EAbilityStopReason
};
struct Json { std::string text; };
struct Control { std::uint8_t code = 0; std::string text; };
inline constexpr std::uint8_t kControlBanner = 10; // host → client: mode status line (empty = clear)

std::size_t encode(const Welcome& m, std::span<std::uint8_t> out);
std::size_t encode(const NameDef& m, std::span<std::uint8_t> out);
std::size_t encode(const MapMsg& m, std::span<std::uint8_t> out);
std::size_t encode(const MapReady& m, std::span<std::uint8_t> out);
std::size_t encode(const Act& m, std::span<std::uint8_t> out);
std::size_t encode(Msg type, const Json& m, std::span<std::uint8_t> out); // Build / EquipRequest / EquipResult
std::size_t encode(const Control& m, std::span<std::uint8_t> out);
std::size_t encode(const Fx& m, std::span<std::uint8_t> out);
std::size_t encode(const FxStop& m, std::span<std::uint8_t> out);
std::size_t encode(const Snd& m, std::span<std::uint8_t> out);
std::size_t encode(const ActStop& m, std::span<std::uint8_t> out);
bool decode(std::span<const std::uint8_t> in, Fx& m);
bool decode(std::span<const std::uint8_t> in, FxStop& m);
bool decode(std::span<const std::uint8_t> in, Snd& m);
bool decode(std::span<const std::uint8_t> in, ActStop& m);

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
