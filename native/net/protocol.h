#pragma once

// Typed coop messages besides the snapshot (§8.2 / §8.3). Pure data + codec (CTest-able).
// Unreliable streams: kSnapStream (host → client snapshot), kInputStream (client → host input).
// Reliable: first byte = Msg type; strings are bounded; every decoder rejects trailing / short input.

#include "net/codec.h"
#include "net/lily_events.h"

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
    Reaction = 15, // ordered Host-accepted Knockback / HitStop / DamageColor for the client's own Lily
    ChildStop = 16, // stop an exact native descendant of a replicated root Act; never a second launch
    LilyFrame = 17, // original consumed input and required movement/state context
    LilyBuild = 18, // authoritative per-seat profile baseline/change
    LilyCauses = 19, // sealed original activation / query / target / completion facts
    RefreshView = 21, // client requests a fresh baseline for its own current map/view
    SpiritFrame = 20, // Host decisions of an autonomous, player-owned original Spirit
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
struct MapMsg { std::uint16_t map = 0; std::string tag; std::uint32_t view = 0; };
struct RefreshView { std::uint16_t map = 0; std::uint32_t view = 0; };
struct MapReady { std::uint16_t map = 0; std::uint32_t view = 0; };
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
    float max_execution_time = 0.0f, damage_rate = 1.0f; // original FAbilityParameters, finite signed values
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
    std::uint8_t flags = 0; // bit0 attach, bit1 follow bone rotation, bit2 running baseline (BaseFX register / active emitter), bit3 emitter (xf = relative rotator/loc/scale), bit4 initial state, bit5 proven Host feedback
    std::int32_t sort = 0;  // BaseFX TranslucencySortPriority; emitter original EAttachLocation (audit)
    std::uint8_t pool = 0;  // Host EPSCPoolMethod (audit); remote replay always uses None
    std::uint8_t attach_kind = 0; // FxAttach; emitter only, no implicit root fallback
    std::uint16_t attach_component = 0; // owner-relative component path; Component/SpineBone only
    std::uint16_t attach_socket = 0; // exact AttachPointName; 0 = None
    std::uint8_t emitter_flags = 0; // bit0 original AutoDestroy, bit1 original AutoActivate
    std::uint32_t t_ms = 0;
    std::uint64_t causal_root = 0;
};
struct FxStop { std::uint32_t seq = 0, t_ms = 0; };
struct Snd {
    std::uint8_t kind = 1;
    std::uint16_t net_id = 0;
    std::uint16_t event = 0; // UFMODEvent path name id
    std::uint8_t is2d = 0;
    std::uint8_t flags = 0; // bit0 proven Host feedback, bit1 exact Host transform; feedback requires bit1 even for is2d
    std::array<float, 10> xf{0, 0, 0, 1, 0, 0, 0, 1, 1, 1}; // quat, translation (map-normalized), scale; serialized only with bit1
    std::uint32_t t_ms = 0;
    std::uint64_t causal_root = 0;
};
struct ActStop {
    std::uint8_t kind = 1;
    std::uint16_t net_id = 0;
    std::uint32_t seq = 0; // the Act.seq of that owner
    std::uint8_t reason = 0; // EAbilityStopReason
};
inline constexpr std::size_t kAbilityPathDepth = 8;
struct AbilityPath {
    std::uint8_t depth = 0; // root = 0; each child edge is a proven container Entries index
    std::array<std::uint16_t, kAbilityPathDepth> entries{};
    std::array<std::uint32_t, kAbilityPathDepth> occurrences{}; // per-entry activation occurrence, starts at 1
    bool operator==(const AbilityPath&) const = default;
};
struct ChildStop {
    std::uint8_t kind = 1;
    std::uint16_t net_id = 0;
    std::uint32_t seq = 0; // original root Act.seq, retained after its Actor stops or is pooled
    std::uint16_t ability = 0; // exact child class path name id
    AbilityPath path;
    std::uint8_t reason = 0; // EAbilityStopReason: 0..3
    bool operator==(const ChildStop&) const = default;
};
bool valid_child_stop(const ChildStop& m) noexcept;
inline constexpr std::size_t kReactionStates = 16;
struct Reaction {
    std::uint8_t kind = 0; // 0 Knockback, 1 HitStop, 2 DamageColor (state_count must be zero)
    std::uint8_t seat = 0;
    std::uint16_t epoch = 0, map = 0;
    std::uint32_t seq = 0, t_ms = 0;
    std::uint8_t state_count = 0;
    std::array<std::uint16_t, kReactionStates> states{};
    float duration = 0, factor = 1; // HitStop: original duration plus verified HitStopFactor context
    // FKnockbackData fields only; never its vptr, UObject pointers or TArray storage.
    std::uint8_t flags = 0; // force restart, reset velocity, absolute vector, until grounded
    std::array<float, 3> ground{}, air{}, swim{};
    float stun = 0, move = 0, strength = 0;
    std::uint16_t curve = 0;
    std::array<float, 3> direction{}, position{}, velocity{}, launch_factor{}, effective_launch{};
    float movement_factor = 1;
    std::uint8_t movement_mode = 0, custom_mode = 0;
    std::uint8_t context = 0; // bit0 previous reaction running, bit1 previous type Launch
    std::uint8_t knockback_type = 0; // original result: Normal=0, Launch=1
    bool operator==(const Reaction&) const = default;
};
bool valid_reaction(const Reaction& m) noexcept;
struct Json { std::string text; };
inline constexpr std::size_t kLilyStates = 48;
inline constexpr std::size_t kLilyClocks = 32;
struct LilyClock { std::uint16_t cls = 0; float scale = 1; bool operator==(const LilyClock&) const = default; };
// Original Lily command lifecycle, in Host execution order within a source frame.
struct LilyCommand {
    std::uint64_t token = 0;
    std::uint32_t input = 0;
    std::uint16_t cls = 0;
    std::uint8_t direction = 0, button = 0, end = 0; // end: 0 = Start, otherwise native reason + 1
    bool operator==(const LilyCommand&) const = default;
};
inline constexpr unsigned kLilyCommands = 32;
inline constexpr std::size_t kLilyFrameBytes = 512 + kLilyCommands * 17;
struct LilyFrame {
    std::vector<LilyCommand> commands;
    std::uint16_t seat = 0, map = 0;
    std::uint16_t epoch = 0;
    std::uint32_t view = 0, t_ms = 0;
    std::uint32_t seq = 0, generation = 0; // Host Pawn serial, distinct from input sequence
    float dt = 0, step = 0, world_dt = 0; // character delta, bounded playout interval, actual Host World delta
    std::array<float, 7> input{}; // left/right axes and original movement vector
    std::uint8_t direction = 0, mode = 0, custom_mode = 0, state_count = 0;
    std::uint16_t flags = 0, raw = 0;
    std::array<float, 3> position{}, velocity{};
    std::array<std::uint16_t, kLilyStates> states{};
    std::uint8_t command_state_count = 0;
    std::array<std::uint16_t, kLilyStates> command_states{};
    std::uint8_t facing = 0, actor_clock_count = 0;
    std::array<LilyClock, kLilyClocks> actor_clocks{};
    std::uint16_t persistent_class = 0;
    std::uint64_t persistent_token = 0;
    std::uint32_t persistent_input = 0;
    std::uint8_t persistent_direction = 0, persistent_button = 0;
    float persistent_elapsed = 0;
    bool operator==(const LilyFrame&) const = default;
};
std::size_t encode(const LilyFrame& m, std::span<std::uint8_t> out);
bool decode(std::span<const std::uint8_t> in, LilyFrame& m);

struct SpiritEdge {
    std::uint32_t sequence = 0;
    std::uint16_t cls = 0;
    std::uint8_t kind = 0, direction = 0, button = 0, reason = 0; // 0 start, 1 end
    bool operator==(const SpiritEdge&) const = default;
};
struct SpiritFrame {
    LilyFrame context;
    std::array<float,7> actor_input{}; // InputComponent, distinct from CommandQueue input in context
    std::array<float,3> movement_input{}; // pending Pawn input consumed separately by CharacterMovement
    std::uint16_t actor_flags = 0, actor_raw = 0;
    std::uint8_t actor_direction = 0, forced_walk = 0;
    std::uint32_t role = 0, episode = 0, summon_input = 0, command_sequence = 0;
    std::uint16_t cls = 0, summon = 0;
    float actor_scale = 1;
    bool active = true;
    bool decision_only = false; // native command boundary, not another World/Actor tick
    bool soft_ending = false; // authoritative original soft-disappearance phase, including its entry edge
    bool has_follow = false;
    std::array<float,3> follow_position{}, follow_rotation{}, follow_velocity{};
    std::array<float,3> follow_scale{1,1,1};
    std::array<float,12> follow_history{}; // blend position, oscillator phases, current/previous target positions
    std::vector<SpiritEdge> edges;
    std::uint8_t layer = 0;
    snap::EventAnimation animation;
    bool operator==(const SpiritFrame&) const = default;
};
std::size_t encode(const SpiritFrame&, std::span<std::uint8_t>);
bool decode(std::span<const std::uint8_t>, SpiritFrame&);
enum class CauseKind : std::uint8_t { Root, Query, Target, End };
struct Cause {
    CauseKind kind = CauseKind::Root;
    std::uint8_t domain = 0; // 0 Lily consumed input, 1 autonomous Spirit accepted command
    std::uint16_t seat = 0, map = 0;
    std::uint32_t generation = 0, input = 0, t_ms = 0;
    std::uint64_t root = 0;
    std::uint16_t ordinal = 0, cls = 0, invoker = 0, query = 0;
    AbilityPath path;
    std::uint8_t ref_kind = 0, flags = 0, channel = 0, reason = 0;
    std::uint16_t ref_id = 0, component = 0;
    std::uint32_t ref_generation = 0;
    float min = 0, range = 0;
    float actor_scale = 1;
    std::array<float, 3> actor_position{}, target_position{}, target_velocity{}, actor_rotation{};
    bool operator==(const Cause&) const = default;
};
inline constexpr std::size_t kCauses = 256;
struct LilyCauses {
    std::uint16_t epoch = 0, map = 0;
    std::uint32_t view = 0, fence_ms = 0;
    std::vector<Cause> facts;
};
std::size_t encode(const LilyCauses& m, std::span<std::uint8_t> out);
bool decode(std::span<const std::uint8_t> in, LilyCauses& m);
struct Control { std::uint8_t code = 0; std::string text; };
inline constexpr std::uint8_t kControlBanner = 10; // host → client: mode status line (empty = clear)

std::size_t encode(const Welcome& m, std::span<std::uint8_t> out);
std::size_t encode(const NameDef& m, std::span<std::uint8_t> out);
std::size_t encode(const MapMsg& m, std::span<std::uint8_t> out);
std::size_t encode(const RefreshView&, std::span<std::uint8_t> out);
bool decode(std::span<const std::uint8_t> in, RefreshView&);
std::size_t encode(const MapReady& m, std::span<std::uint8_t> out);
std::size_t encode(const Act& m, std::span<std::uint8_t> out);
std::size_t encode(Msg type, const Json& m, std::span<std::uint8_t> out); // Build / EquipRequest / EquipResult
std::size_t encode(const Control& m, std::span<std::uint8_t> out);
std::size_t encode(const Fx& m, std::span<std::uint8_t> out);
std::size_t encode(const FxStop& m, std::span<std::uint8_t> out);
std::size_t encode(const Snd& m, std::span<std::uint8_t> out);
std::size_t encode(const ActStop& m, std::span<std::uint8_t> out);
std::size_t encode(const ChildStop& m, std::span<std::uint8_t> out);
std::size_t encode(const Reaction& m, std::span<std::uint8_t> out);
bool decode(std::span<const std::uint8_t> in, Fx& m);
bool decode(std::span<const std::uint8_t> in, FxStop& m);
bool decode(std::span<const std::uint8_t> in, Snd& m);
bool decode(std::span<const std::uint8_t> in, ActStop& m);
bool decode(std::span<const std::uint8_t> in, ChildStop& m);
bool decode(std::span<const std::uint8_t> in, Reaction& m);

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
