#include "coop/reaction.h"
#include "coop/reaction_color.h"

#include "base/clock.h"
#include "base/hook.h"
#include "base/log.h"
#include "base/preflight.h"
#include "engine/reflect.h"
#include "engine/world.h"
#include "guard/guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <optional>

namespace elmp::coop::reaction {
namespace {
using Vec = std::array<float, 3>;
using LaunchFn = std::int32_t(__fastcall*)(ue::UObject*, const void*, Vec*);
using HitStopFn = void(__fastcall*)(ue::UObject*, float);
LaunchFn o_launch{};
HitStopFn o_hitstop{};
void (*g_sink)(const Capture&) = nullptr;
bool (*g_filter)(ue::UObject*) = nullptr;
bool g_installed = false, g_client = false;
bool g_hitstop_installed = false;
ObjHandle g_knockback_allowed;
bool g_knockback_allow_once = false;
ObjHandle g_hitstop_allowed;
bool g_hitstop_allow_once = false;
std::uint64_t g_entry_order = 0;
std::uint64_t g_captured = 0, g_capture_failed = 0, g_native_refused = 0, g_replayed = 0, g_rejected = 0, g_local_blocked = 0;
std::uint64_t g_hitstop_captured = 0, g_hitstop_refused = 0, g_hitstop_replayed = 0, g_hitstop_local_blocked = 0;
std::uint64_t g_anchor_no_move = 0, g_anchor_failed = 0;
std::string g_failure;
std::string g_host_fault;

struct Schema {
    std::vector<ObjHandle> types;
    reflect::PropRef restart, reset_velocity, ground, air, swim, absolute, until_ground, stun, move, strength, curve, states;
    reflect::PropRef owner, runtime, runtime_type, runtime_launch, movement_factor, launch_factor;
    reflect::PropRef character_movement, velocity, mode, custom_mode;
    reflect::FuncRef by_class, launch, running, get_location, set_location, set_mode;
    ObjHandle knockback_class, curve_class, state_class, class_class;
    bool live() const {
        return std::all_of(types.begin(), types.end(), [](const auto& h) { return h.valid(); }) &&
               launch.handle.valid() && running.handle.valid() && by_class.handle.valid() &&
               get_location.handle.valid() && set_location.handle.valid() && set_mode.handle.valid();
    }
};
// /Script schemas only; lifetime is their weak serials, explicit reset, or a failed resolve.
// No component/asset raw-pointer cache survives a call. Failed cold resolution is retried.
std::optional<Schema> g_schema;

struct HitStopSchema {
    std::vector<ObjHandle> types;
    ObjHandle component_class, state_class, class_class, spine_class, state_component_class, movement_class;
    reflect::PropRef factor, states, spine, state_component, movement;
    reflect::FuncRef launch, running, by_class;
    bool live() const {
        return std::all_of(types.begin(), types.end(), [](const auto& h) { return h.valid(); }) &&
               launch.handle.valid() && running.handle.valid() && by_class.handle.valid();
    }
};
// Independent native schema. No actor/component raw pointers survive a call; class serials and reset()
// invalidate every cached offset/function. A failed resolution is retried and appears in describe().
std::optional<HitStopSchema> g_hitstop_schema;

const HitStopSchema* hitstop_schema() {
    if (g_hitstop_schema && g_hitstop_schema->live()) return &*g_hitstop_schema;
    g_hitstop_schema.reset();
    HitStopSchema s;
    auto type = [&](const wchar_t* path) {
        auto* p = reflect::find_class(path);
        s.types.push_back(ObjHandle::of(p)); return p;
    };
    auto* hs = type(L"/Script/Zenith.HitStopComponent");
    auto* actor = type(L"/Script/Engine.Actor");
    s.component_class = ObjHandle::of(hs);
    s.state_class = ObjHandle::of(type(L"/Script/Zenith.State"));
    s.class_class = ObjHandle::of(type(L"/Script/CoreUObject.Class"));
    s.spine_class = ObjHandle::of(type(L"/Script/SpinePlugin.SpineSkeletonAnimationComponent"));
    s.state_component_class = ObjHandle::of(type(L"/Script/Zenith.StateComponent"));
    s.movement_class = ObjHandle::of(type(L"/Script/Engine.MovementComponent"));
    reflect::Resolver h("ReactionHitStop", hs), a("ReactionHitStop Actor", actor);
    s.factor = h.prop(L"HitStopFactor", reflect::Kind::Float, 4);
    s.states = h.prop(L"AdditionalHitStopStates", reflect::Kind::Array, 16);
    s.spine = h.prop(L"SpineAnimComponent", reflect::Kind::Object, 8);
    s.state_component = h.prop(L"StateComponent", reflect::Kind::Object, 8);
    s.movement = h.prop(L"MovementComponent", reflect::Kind::Object, 8);
    s.launch = h.func(L"LaunchHitStop", 1);
    s.running = h.func(L"IsInHitStop", 0);
    s.by_class = a.func(L"GetComponentByClass", 1);
    if (!h.ok() || !a.ok()) { g_failure = h.failure() + a.failure(); return nullptr; }
    if (reflect::properties_size(hs) != 272 || reflect::element_size(s.states) != 8 ||
        s.launch.ret >= 0 || s.launch.parms[0].kind != reflect::Kind::Float || s.launch.parms[0].size != 4 ||
        s.running.ret < 0 || s.running.parms[s.running.ret].kind != reflect::Kind::Bool || !s.live()) {
        g_failure = "HitStop reflected class/float ABI/AdditionalStates layout mismatch"; return nullptr;
    }
    g_hitstop_schema = std::move(s); return &*g_hitstop_schema;
}

bool hitstop_context(const HitStopSchema& s, ue::UObject* comp, ue::UObject* actor) {
    auto matches = [&](ue::UObject* object, const ObjHandle& cls) {
        return ue::alive(object) && object->GetOuterPrivate() == actor &&
               object->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(cls.get()));
    };
    if (!ue::alive(actor) || !matches(comp, s.component_class) ||
        !matches(s.spine.object(comp), s.spine_class) ||
        !matches(s.state_component.object(comp), s.state_component_class) ||
        !matches(s.movement.object(comp), s.movement_class)) {
        g_failure = "HitStop component/cached Spine/State/Movement is not initialized for the same actor";
        return false;
    }
    return true;
}

bool hitstop_state_paths(const HitStopSchema& s, ue::UObject* comp, std::vector<std::string>& out) {
    const auto& states = reflect::array_at(comp, s.states);
    if (states.num < 0 || states.num > static_cast<int>(net::proto::kReactionStates) || states.max < states.num || (states.num && !states.data)) {
        g_failure = "HitStop AdditionalStates exceeds bounded valid array"; return false;
    }
    out.clear();
    for (int i = 0; i < states.num; ++i) {
        ue::UObject* object = nullptr; std::memcpy(&object, states.data + i * sizeof(object), sizeof(object));
        if (!ue::alive(object) || !object->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(s.class_class.get())) ||
            !static_cast<ue::UClass*>(object)->IsChildOf(static_cast<ue::UClass*>(s.state_class.get()))) {
            g_failure = "HitStop AdditionalStates contains a null/expired/non-State class"; return false;
        }
        out.push_back(log::utf8(object->GetPathName()));
    }
    return true;
}

bool hitstop_running(const HitStopSchema& s, ue::UObject* comp, bool& value) {
    reflect::Frame f(s.running);
    if (!f.invoke(comp)) { g_failure = "HitStop IsInHitStop unavailable"; return false; }
    value = f.ret_bool(); return true;
}

const Schema* schema() {
    if (g_schema && g_schema->live()) return &*g_schema;
    g_schema.reset();
    Schema s;
    auto type = [&](const wchar_t* path) {
        auto* p = static_cast<ue::UStruct*>(reflect::find_object(path));
        s.types.push_back(ObjHandle::of(p)); return p;
    };
    auto* data = type(L"/Script/Zenith.KnockbackData");
    auto* runtime = type(L"/Script/Zenith.KnockbackRuntimeData");
    auto* kb = type(L"/Script/Zenith.KnockbackComponent");
    auto* actor = type(L"/Script/Engine.Actor");
    auto* character = type(L"/Script/Engine.Character");
    auto* movement = type(L"/Script/Engine.CharacterMovementComponent");
    s.curve_class = ObjHandle::of(type(L"/Script/Engine.CurveFloat"));
    s.state_class = ObjHandle::of(type(L"/Script/Zenith.State"));
    s.class_class = ObjHandle::of(type(L"/Script/CoreUObject.Class"));
    if (reflect::properties_size(data) != 88 || reflect::properties_size(runtime) != 136) {
        g_failure = "KnockbackData/RuntimeData reflected size differs from verified 88/136"; return nullptr;
    }
    reflect::Resolver d("ReactionData", data), k("ReactionComponent", kb), r("ReactionRuntime", runtime);
    reflect::Resolver a("ReactionActor", actor), ch("ReactionCharacter", character), cm("ReactionMovement", movement);
    s.restart = d.prop(L"bForceRestartDamageAnimation", reflect::Kind::Bool);
    s.reset_velocity = d.prop(L"bResetVelocity", reflect::Kind::Bool);
    s.ground = d.prop(L"LaunchVectorGrounded", reflect::Kind::Struct, 12);
    s.air = d.prop(L"LaunchVectorAir", reflect::Kind::Struct, 12);
    s.swim = d.prop(L"LaunchVectorSwim", reflect::Kind::Struct, 12);
    s.absolute = d.prop(L"bAbsoluteLaunchVector", reflect::Kind::Bool);
    s.until_ground = d.prop(L"bKnockbackUntilGrounded", reflect::Kind::Bool);
    s.stun = d.prop(L"StunTime", reflect::Kind::Float);
    s.move = d.prop(L"MoveTime", reflect::Kind::Float);
    s.strength = d.prop(L"Strength", reflect::Kind::Float);
    s.curve = d.prop(L"MovementCurve", reflect::Kind::Object);
    s.states = d.prop(L"AdditionalStates", reflect::Kind::Array, 16);
    s.owner = k.prop(L"OwnerCharacter", reflect::Kind::Object);
    s.runtime = k.prop(L"RunningKnockback", reflect::Kind::Struct, 136);
    s.movement_factor = k.prop(L"MovementFactor", reflect::Kind::Float);
    s.launch_factor = k.prop(L"LaunchKnockbackFactor", reflect::Kind::Struct, 12);
    s.runtime_type = r.prop(L"KnockbackType", reflect::Kind::Any, 1);
    s.runtime_launch = r.prop(L"LaunchVector", reflect::Kind::Struct, 12);
    s.character_movement = ch.prop(L"CharacterMovement", reflect::Kind::Object);
    s.velocity = cm.prop(L"Velocity", reflect::Kind::Struct, 12);
    s.mode = cm.prop(L"MovementMode", reflect::Kind::Any, 1);
    s.custom_mode = cm.prop(L"CustomMovementMode", reflect::Kind::Byte, 1);
    s.by_class = a.func(L"GetComponentByClass", 1);
    s.get_location = a.func(L"K2_GetActorLocation", 0);
    s.set_location = a.func(L"K2_SetActorLocation", 4);
    s.set_mode = cm.func(L"SetMovementMode", 2);
    s.launch = k.func(L"LaunchKnockback", 2);
    s.running = k.func(L"IsInKnockback", 0);
    s.knockback_class = ObjHandle::of(kb);
    if (!d.ok() || !k.ok() || !r.ok() || !a.ok() || !ch.ok() || !cm.ok()) {
        g_failure = d.failure() + k.failure() + r.failure() + a.failure() + ch.failure() + cm.failure(); return nullptr;
    }
    if (s.launch.parms[0].size != 88 || s.launch.parms[1].size != 12 || s.launch.ret < 0 ||
        s.launch.parms[s.launch.ret].size != 4 || reflect::element_size(s.states) != 8 || !s.live()) {
        g_failure = "LaunchKnockback reflected ABI/AdditionalStates element mismatch"; return nullptr;
    }
    g_schema = std::move(s); return &*g_schema;
}

bool is_running(const Schema& s, ue::UObject* comp, bool& value) {
    reflect::Frame f(s.running);
    if (!f.invoke(comp)) return false;
    value = f.ret_bool(); return true;
}
bool close_float(float a, float b) { return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= 0.001f * std::max({1.0f, std::fabs(a), std::fabs(b)}); }
bool close_vec(const Vec& a, const Vec& b) {
    for (std::size_t i = 0; i < a.size(); ++i) if (!close_float(a[i], b[i])) return false;
    return true;
}

bool capture_before(const Schema& s, ue::UObject* comp, const void* data, const Vec& direction, Capture& cap) {
    cap.owner = s.owner.object(comp);
    auto* movement = cap.owner ? s.character_movement.object(cap.owner) : nullptr;
    if (!movement) { g_failure = "capture missing OwnerCharacter/CharacterMovement"; return false; }
    auto& e = cap.event;
    e.t_ms = static_cast<std::uint32_t>(clock::now_us() / 1000);
    e.flags = (s.restart.read_bool(data) ? 1 : 0) | (s.reset_velocity.read_bool(data) ? 2 : 0) |
              (s.absolute.read_bool(data) ? 4 : 0) | (s.until_ground.read_bool(data) ? 8 : 0);
    e.ground = s.ground.read<Vec>(data); e.air = s.air.read<Vec>(data); e.swim = s.swim.read<Vec>(data);
    e.stun = s.stun.read<float>(data); e.move = s.move.read<float>(data); e.strength = s.strength.read<float>(data);
    const auto* curve = s.curve.read<ue::UObject*>(data);
    if (curve && !ue::alive(curve)) { g_failure = "capture expired movement curve"; return false; }
    if (curve) cap.curve = log::utf8(curve->GetPathName());
    const auto& states = reflect::array_at(data, s.states);
    if (states.num < 0 || states.num > static_cast<int>(net::proto::kReactionStates) || states.max < states.num || (states.num && !states.data)) {
        g_failure = "capture AdditionalStates exceeds bounded valid array"; return false;
    }
    for (int i = 0; i < states.num; ++i) {
        ue::UObject* cls = nullptr; std::memcpy(&cls, states.data + i * sizeof(cls), sizeof(cls));
        if (!ue::alive(cls)) { g_failure = "capture null/expired AdditionalState class"; return false; }
        cap.states.push_back(log::utf8(cls->GetPathName()));
    }
    e.state_count = static_cast<std::uint8_t>(cap.states.size());
    e.direction = direction;
    e.velocity = s.velocity.read<Vec>(movement);
    e.movement_mode = s.mode.read<std::uint8_t>(movement); e.custom_mode = s.custom_mode.read<std::uint8_t>(movement);
    e.movement_factor = s.movement_factor.read<float>(comp); e.launch_factor = s.launch_factor.read<Vec>(comp);
    reflect::Frame at(s.get_location);
    if (!at.invoke(cap.owner)) { g_failure = "capture actor location"; return false; }
    e.position = at.ret<Vec>();
    bool running = false;
    if (!is_running(s, comp, running)) return false;
    if (running) e.context = 1 | (s.runtime_type.read<std::uint8_t>(s.runtime.at<void>(comp)) == 1 ? 2 : 0);
    return true;
}

std::int32_t __fastcall d_launch(ue::UObject* comp, const void* data, Vec* direction) {
    auto* outer = ue::alive(comp) ? comp->GetOuterPrivate() : nullptr;
    if (g_client && guard::armed() && outer && outer == world::p1_pawn()) {
        if (!g_knockback_allow_once || g_knockback_allowed.get() != comp) { ++g_local_blocked; return -1; }
        g_knockback_allow_once = false;
    }
    if (!g_sink || !ue::on_game_thread() || !data || !direction || !outer || (g_filter && !g_filter(outer)))
        return o_launch(comp, data, direction);
    const auto* s = schema();
    Capture cap;
    cap.entry_order = next_capture_order();
    const bool ready = cap.entry_order && s && capture_before(*s, comp, data, *direction, cap);
    const auto result = o_launch(comp, data, direction);
    if (result < 0) { ++g_native_refused; return result; }
    if (!ready) {
        ++g_capture_failed;
        if (g_host_fault.empty()) g_host_fault = g_failure.empty() ? "accepted native reaction capture failed" : g_failure;
        return result;
    }
    const auto* runtime = s->runtime.at<void>(comp);
    cap.event.knockback_type = s->runtime_type.read<std::uint8_t>(runtime);
    cap.event.effective_launch = s->runtime_launch.read<Vec>(runtime);
    auto validated = cap.event;
    validated.seat = 1; validated.epoch = 1; validated.map = 1; validated.seq = 1;
    for (std::size_t i = 0; i < validated.state_count; ++i) validated.states[i] = 1;
    if (!net::proto::valid_reaction(validated)) {
        ++g_capture_failed;
        if (g_host_fault.empty()) g_host_fault = "accepted native reaction exceeds validated wire fields";
        return result;
    }
    ++g_captured;
    if (g_sink) g_sink(cap);
    return result;
}

void __fastcall d_hitstop(ue::UObject* comp, float duration) {
    auto* outer = ue::alive(comp) ? comp->GetOuterPrivate() : nullptr;
    if (g_client && guard::armed() && outer && outer == world::p1_pawn()) {
        if (!g_hitstop_allow_once || g_hitstop_allowed.get() != comp) { ++g_hitstop_local_blocked; return; }
        // A nested original callback receives its own ordered Host event. It must not borrow the
        // outer reaction's permission and execute once locally plus once from the wire.
        g_hitstop_allow_once = false;
    }
    if (!g_sink || !ue::on_game_thread() || !outer || (g_filter && !g_filter(outer))) {
        o_hitstop(comp, duration); return;
    }
    const auto* s = hitstop_schema();
    Capture cap;
    cap.entry_order = next_capture_order();
    cap.owner = outer;
    auto& e = cap.event;
    e.kind = 1;
    e.t_ms = static_cast<std::uint32_t>(clock::now_us() / 1000);
    e.duration = duration;
    if (s) {
        e.factor = s->factor.read<float>(comp);
        // E-IDA CADAA0: this is the native admission predicate. The stored duration is NOT
        // multiplied by the factor. Repeated accepted calls reset elapsed and broadcast again.
        if (!(duration * e.factor > 0.0f)) {
            ++g_hitstop_refused; o_hitstop(comp, duration); return;
        }
    }
    bool ready = cap.entry_order && s && hitstop_context(*s, comp, outer) && hitstop_state_paths(*s, comp, cap.states);
    e.state_count = static_cast<std::uint8_t>(cap.states.size());
    auto validated = e;
    validated.seat = 1; validated.epoch = 1; validated.map = 1; validated.seq = 1;
    for (std::size_t i = 0; i < validated.state_count; ++i) validated.states[i] = 1;
    if (ready && !net::proto::valid_reaction(validated)) {
        g_failure = "accepted HitStop duration/factor exceeds validated wire fields";
        ready = false;
    }
    o_hitstop(comp, duration);
    bool running = false;
    if (ready && (!hitstop_running(*s, comp, running) || !running)) {
        g_failure = "accepted HitStop did not remain running after native start";
        ready = false;
    }
    if (!ready) {
        ++g_capture_failed;
        if (g_host_fault.empty()) g_host_fault = g_failure.empty() ? "accepted HitStop capture failed" : g_failure;
        return;
    }
    ++g_captured; ++g_hitstop_captured;
    if (g_sink) g_sink(cap);
}

bool install() {
    if (g_installed) return true;
    const auto* s = schema();
    if (!s) return false;
    auto* exec = reinterpret_cast<const std::uint8_t*>(s->launch.fn->GetFuncPtr());
    if (!exec || exec != preflight::address("ReactionLaunchKnockbackExec") || exec[0x139] != 0xE8) {
        g_failure = "reflected LaunchKnockback exec/direct-call mismatch"; return false;
    }
    std::int32_t displacement = 0; std::memcpy(&displacement, exec + 0x13A, sizeof(displacement));
    auto* target = const_cast<std::uint8_t*>(exec + 0x13E + displacement);
    if (target != preflight::address("ReactionLaunchKnockback")) {
        g_failure = "reflected LaunchKnockback target differs from verified manifest"; return false;
    }
    g_installed = hook::install_address("ReactionLaunchKnockback", target, reinterpret_cast<void*>(&d_launch), reinterpret_cast<void**>(&o_launch), "reflect");
    if (!g_installed) g_failure = "native LaunchKnockback detour refused";
    return g_installed;
}

bool install_hitstop() {
    if (g_hitstop_installed) return true;
    const auto* s = hitstop_schema();
    if (!s) return false;
    auto* exec = reinterpret_cast<const std::uint8_t*>(s->launch.fn->GetFuncPtr());
    if (!exec || exec != preflight::address("ReactionLaunchHitStopExec") || exec[0x70] != 0xE8) {
        g_failure = "reflected LaunchHitStop exec/direct-call mismatch"; return false;
    }
    std::int32_t displacement = 0; std::memcpy(&displacement, exec + 0x71, sizeof(displacement));
    auto* target = const_cast<std::uint8_t*>(exec + 0x75 + displacement);
    if (target != preflight::address("ReactionLaunchHitStop")) {
        g_failure = "reflected LaunchHitStop target differs from verified manifest"; return false;
    }
    g_hitstop_installed = hook::install_address("ReactionLaunchHitStop", target, reinterpret_cast<void*>(&d_hitstop), reinterpret_cast<void**>(&o_hitstop), "reflect");
    if (!g_hitstop_installed) g_failure = "native LaunchHitStop detour refused";
    return g_hitstop_installed;
}

bool replay_hitstop(ue::UObject* actor, const net::proto::Reaction& e, NameLookup lookup) {
    const auto* s = hitstop_schema();
    if (!s) return false;
    reflect::Frame find(s->by_class); find.arg(0, s->component_class.get());
    auto* comp = find.invoke(actor) ? find.ret<ue::UObject*>() : nullptr;
    if (!hitstop_context(*s, comp, actor)) return false;
    const float factor = s->factor.read<float>(comp);
    if (!(factor > 0.0f) || !std::isfinite(e.duration * factor) || !(e.duration * factor > 0.0f) || !close_float(factor, e.factor)) {
        g_failure = "Host/local HitStopFactor differs"; return false;
    }
    std::vector<std::string> local_states;
    if (!hitstop_state_paths(*s, comp, local_states)) return false;
    if (local_states.size() != e.state_count) {
        g_failure = "Host/local HitStop AdditionalStates count differs"; return false;
    }
    for (std::size_t i = 0; i < local_states.size(); ++i) {
        const auto* path = lookup ? lookup(e.states[i]) : nullptr;
        if (!path || *path != local_states[i]) {
            g_failure = std::format("Host/local HitStop AdditionalStates[{}] unresolved or differs", i); return false;
        }
    }
    // Reuse the initialized local component and its original states. Never fabricate a paused pose,
    // overwrite its arrays, or derive HitStop from the preceding Knockback/HP snapshot.
    reflect::Frame call(s->launch); call.arg(0, e.duration);
    struct ReplayScope {
        ObjHandle prior = g_hitstop_allowed;
        bool pending = g_hitstop_allow_once;
        explicit ReplayScope(ue::UObject* component) { g_hitstop_allowed = ObjHandle::of(component); g_hitstop_allow_once = true; }
        ~ReplayScope() { g_hitstop_allowed = prior; g_hitstop_allow_once = pending; }
    } scope(comp);
    if (!call.invoke(comp)) { g_failure = "native LaunchHitStop invocation failed"; return false; }
    if (g_hitstop_allow_once) { g_failure = "LaunchHitStop did not enter the verified native hook"; return false; }
    bool running = false;
    if (!hitstop_running(*s, comp, running) || !running) {
        g_failure = "native LaunchHitStop did not enter running state"; return false;
    }
    ++g_hitstop_replayed; return true;
}

ue::UObject* load_named(NameLookup lookup, std::uint16_t id, ue::UClass* expected, bool is_class) {
    const auto* path = lookup ? lookup(id) : nullptr;
    if (!path || path->empty() || path->size() > net::proto::kMaxName || path->front() != '/') return nullptr;
    auto* object = reflect::load_object(log::wide(*path));
    if (!ue::alive(object) || !expected) return nullptr;
    if (is_class) {
        auto* meta = static_cast<ue::UClass*>(g_schema->class_class.get());
        if (!meta || !object->GetClassPrivate()->IsChildOf(meta)) return nullptr;
        return static_cast<ue::UClass*>(object)->IsChildOf(expected) ? object : nullptr;
    }
    return object->GetClassPrivate()->IsChildOf(expected) ? object : nullptr;
}
} // namespace

std::uint64_t next_capture_order() {
    if (g_entry_order == std::numeric_limits<std::uint64_t>::max()) {
        if (g_host_fault.empty()) g_host_fault = "native reaction entry order exhausted";
        return 0;
    }
    return ++g_entry_order;
}

bool host_enable(void (*sink)(const Capture&), bool (*filter)(ue::UObject*)) {
    if (!install() || !install_hitstop() || !color::host_enable(sink, filter)) return false;
    g_sink = sink; g_filter = filter; return true;
}
void host_disable() { g_sink = nullptr; g_filter = nullptr; g_host_fault.clear(); color::host_disable(); }
std::string host_fault() { return g_host_fault.empty() ? color::host_fault() : g_host_fault; }
bool client_enable(bool on) {
    if (on && (!install() || !install_hitstop())) return false;
    if (!color::client_enable(on)) return false;
    g_client = on; return true;
}

bool replay(ue::UObject* actor, const net::proto::Reaction& e, NameLookup lookup, std::string* why) {
    const auto fail = [&](std::string text) {
        g_failure = std::move(text); ++g_rejected; if (why) *why = g_failure; return false;
    };
    if (!g_client || !guard::armed() || !ue::on_game_thread() || !ue::alive(actor) || actor != world::p1_pawn())
        return fail("reaction requires armed Client's controlled Lily on game thread");
    if (!net::proto::valid_reaction(e)) return fail("malformed Reaction");
    if (e.kind == 1) {
        if (!replay_hitstop(actor, e, lookup)) return fail(g_failure);
        ++g_replayed; return true;
    }
    if (e.kind == 2) {
        std::string reason;
        if (!color::replay(actor, e, &reason)) return fail(reason);
        ++g_replayed; return true;
    }
    const auto* s = schema();
    if (!s) return fail(g_failure);
    reflect::Frame find(s->by_class); find.arg(0, s->knockback_class.get());
    auto* comp = find.invoke(actor) ? find.ret<ue::UObject*>() : nullptr;
    auto* movement = s->character_movement.object(actor);
    if (!ue::alive(comp) || s->owner.object(comp) != actor || !movement) return fail("Knockback component is not initialized for controlled Lily");
    if (!close_float(s->movement_factor.read<float>(comp), e.movement_factor) || !close_vec(s->launch_factor.read<Vec>(comp), e.launch_factor))
        return fail("Host/local native Knockback factors differ");
    bool running = false;
    if (!is_running(*s, comp, running)) return fail("IsInKnockback unavailable");
    const bool previous_launch = running && s->runtime_type.read<std::uint8_t>(s->runtime.at<void>(comp)) == 1;
    if (previous_launch != ((e.context & 2) != 0) && e.knockback_type == 0)
        return fail("previous Launch context would change Host Normal reaction");
    ue::UObject* curve = nullptr;
    if (e.curve && !(curve = load_named(lookup, e.curve, static_cast<ue::UClass*>(s->curve_class.get()), false))) return fail("movement curve asset unresolved/wrong class");
    std::array<ue::UObject*, net::proto::kReactionStates> states{};
    for (std::size_t i = 0; i < e.state_count; ++i)
        if (!(states[i] = load_named(lookup, e.states[i], static_cast<ue::UClass*>(s->state_class.get()), true)))
            return fail(std::format("AdditionalStates[{}] unresolved/not State class", i));
    reflect::Frame call(s->launch);
    auto* data = call.raw(0);
    if (!data) return fail("LaunchKnockback parameter frame");
    s->restart.write_bool(data, e.flags & 1); s->reset_velocity.write_bool(data, e.flags & 2);
    s->absolute.write_bool(data, e.flags & 4); s->until_ground.write_bool(data, e.flags & 8);
    *s->ground.at<Vec>(data) = e.ground; *s->air.at<Vec>(data) = e.air; *s->swim.at<Vec>(data) = e.swim;
    *s->stun.at<float>(data) = e.stun; *s->move.at<float>(data) = e.move; *s->strength.at<float>(data) = e.strength;
    *s->curve.at<ue::UObject*>(data) = curve;
    // The UFunction owns its initialized struct; temporarily borrow this bounded class-pointer array.
    // Native Launch deep-copies AdditionalStates into RunningKnockback. Unborrow before Frame destroys it.
    auto* array = s->states.at<reflect::RawArray>(data);
    struct Borrow {
        reflect::RawArray* array;
        ~Borrow() { *array = {}; }
    } borrowed{array};
    *array = {reinterpret_cast<std::uint8_t*>(states.data()), e.state_count, e.state_count};
    call.arg(1, e.direction);
    // Use the actual Host mode/velocity, not a fabricated grounded flag or a modified KnockbackData.
    reflect::Frame at(s->set_location); at.arg(0, e.position).arg_bool(1, false).arg_bool(3, true);
    const bool invoked = at.invoke(actor);
    const bool native_moved = invoked && at.ret_bool();
    reflect::Frame located(s->get_location);
    if (!located.invoke(actor)) {
        ++g_anchor_failed;
        return fail(std::format("Host reaction anchor unreadable: invoked={} nativeRet={} expected=({},{},{})",
            invoked, native_moved, e.position[0], e.position[1], e.position[2]));
    }
    const auto actual = located.ret<Vec>();
    // UE PrimitiveComponent returns bMoved, not 'destination reached': a tiny nonzero delta may
    // reach InternalSetWorldLocationAndRotation and be considered equal without a transform write.
    // Use a small absolute world-space tolerance; close_vec's relative tolerance is inappropriate here.
    float anchor_error = 0;
    bool finite = true;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        finite = finite && std::isfinite(actual[i]);
        anchor_error = std::max(anchor_error, std::fabs(actual[i] - e.position[i]));
    }
    const bool placed = invoked && finite && anchor_error <= 0.01f;
    if (!native_moved || !placed) {
        const auto detail = std::format("Host reaction anchor: invoked={} nativeRet={} actual=({},{},{}) expected=({},{},{}) maxAbsError={} finite={} failed={}",
            invoked, native_moved, actual[0], actual[1], actual[2], e.position[0], e.position[1], e.position[2], anchor_error, finite, !placed);
        if (!placed) { ++g_anchor_failed; return fail(detail); }
        ++g_anchor_no_move;
        log::info("reaction: {}", detail);
    }
    // A mode transition can find the floor at the current location: establish the Host anchor first.
    reflect::Frame mode(s->set_mode); mode.arg(0, e.movement_mode).arg(1, e.custom_mode);
    if (!mode.invoke(movement)) return fail("SetMovementMode failed");
    if (s->mode.read<std::uint8_t>(movement) != e.movement_mode || s->custom_mode.read<std::uint8_t>(movement) != e.custom_mode)
        return fail("native movement mode could not reproduce Host context");
    *s->velocity.at<Vec>(movement) = e.velocity;
    struct ReplayScope {
        ObjHandle prior = g_knockback_allowed;
        bool pending = g_knockback_allow_once;
        explicit ReplayScope(ue::UObject* component) { g_knockback_allowed = ObjHandle::of(component); g_knockback_allow_once = true; }
        ~ReplayScope() { g_knockback_allowed = prior; g_knockback_allow_once = pending; }
    } scope(comp);
    if (!call.invoke(comp) || call.ret<std::int32_t>() < 0) return fail("native LaunchKnockback rejected Host event");
    if (g_knockback_allow_once) return fail("LaunchKnockback did not enter the verified native hook");
    const auto* runtime = s->runtime.at<void>(comp);
    if (s->runtime_type.read<std::uint8_t>(runtime) != e.knockback_type || !close_vec(s->runtime_launch.read<Vec>(runtime), e.effective_launch))
        return fail("native replay Type/LaunchVector differs from Host result");
    ++g_replayed; return true;
}

bool active(ue::UObject* actor) {
    if (!g_client || !actor || actor != world::p1_pawn()) return false;
    const auto* hs = hitstop_schema();
    if (!hs) return true;
    reflect::Frame find_hitstop(hs->by_class); find_hitstop.arg(0, hs->component_class.get());
    auto* hitstop = find_hitstop.invoke(actor) ? find_hitstop.ret<ue::UObject*>() : nullptr;
    bool stopped = false;
    if (!hitstop_context(*hs, hitstop, actor) || !hitstop_running(*hs, hitstop, stopped) || stopped) return true;
    const auto* s = schema();
    if (!s) return true;
    reflect::Frame find(s->by_class); find.arg(0, s->knockback_class.get());
    auto* comp = find.invoke(actor) ? find.ret<ue::UObject*>() : nullptr;
    if (!ue::alive(comp) || s->owner.object(comp) != actor) return true;
    bool running = false;
    // E-IDA: IsInKnockback CAC0D0 is +417; IsInKnockbackRecovery CAC0E0 is +417 && +552.
    // Therefore the former already covers native recovery, without holding after the component ends.
    return !is_running(*s, comp, running) || running;
}

void reset() {
    g_schema.reset(); g_hitstop_schema.reset();
    g_knockback_allowed = {}; g_knockback_allow_once = false;
    g_hitstop_allowed = {}; g_hitstop_allow_once = false;
    color::reset();
}
std::string describe() {
    return std::format("reaction: hook={} host={} client={} captured={} captureFailed={} nativeRefused={} replayed={} rejected={} spontaneousBlocked={} anchorNoMoveAccepted={} anchorFailed={} last={}\n"
                       "  hitstop: hook={} captured={} refused={} replayed={} spontaneousBlocked={} entryOrder={}\n",
        g_installed, g_sink != nullptr, g_client, g_captured, g_capture_failed, g_native_refused, g_replayed, g_rejected, g_local_blocked, g_anchor_no_move, g_anchor_failed, g_failure,
        g_hitstop_installed, g_hitstop_captured, g_hitstop_refused, g_hitstop_replayed, g_hitstop_local_blocked, g_entry_order) + color::describe();
}
} // namespace elmp::coop::reaction
