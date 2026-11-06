#include "coop/native_lily.h"
#include "coop/session.h"
#include "coop/ability.h"
#include "coop/causality.h"
#include "core/player.h"
#include "core/profile.h"
#include "core/schema.h"
#include "core/spawn.h"
#include "core/skin.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/world.h"
#include "input/virtual_pad.h"
#include "guard/guard.h"
#include "base/json.h"
#include "base/log.h"
#include "base/hook.h"
#include "base/clock.h"
#include "base/metrics.h"
#include <unordered_map>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>

namespace elmp::coop::native_lily {
namespace {
namespace proto = net::proto;
struct Schema {
    ObjHandle actor, character, movement, state;
    ObjHandle companion_ai;
    reflect::PropRef dilation, velocity, custom_mode, states, state_component;
    reflect::FuncRef add_state, remove_state, get_owner, get_controller;
    reflect::FuncRef leader;
    ObjHandle gameplay;
    reflect::FuncRef world_delta;
    bool ready() const { return actor.valid() && character.valid() && movement.valid() && state.valid() && gameplay.valid() && world_delta.ok(); }
};
Schema g_schema;
std::string g_fault;
float g_dt = 1.0f / 60.0f;
float g_source_dt = 1.0f / 60.0f;
Text g_text = nullptr;
using CharacterTick = void(__fastcall*)(ue::UObject*, float);
CharacterTick g_original_tick = nullptr;
std::unordered_map<std::uint16_t, ObjHandle> g_state_classes;
FrameSink g_sink = nullptr;
Name g_name = nullptr;
std::array<std::uint32_t, 9> g_sequence{};
ObjHandle g_executing_pawn;
struct ExecutionScope { ObjHandle previous=g_executing_pawn; explicit ExecutionScope(ue::UObject* pawn){g_executing_pawn=ObjHandle::of(pawn);}~ExecutionScope(){g_executing_pawn=previous;} };
struct SourceCadence {
    std::uint32_t generation = 0;
    std::array<float, 31> samples{};
    std::size_t next = 0;
    float interval(std::uint32_t serial, float dt) {
        if (generation != serial) { generation = serial; samples.fill(1.0f / 60); next = 0; }
        samples[next++ % samples.size()] = dt;
        auto sorted = samples;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        return std::min(dt, sorted[sorted.size() / 2] * 1.5f);
    }
};
std::array<SourceCadence, 9> g_source_cadence{};
struct Replica {
    struct Step { proto::LilyFrame frame; bool begin = false, end = false; double host_ms = 0; };
    ObjHandle pawn, controller, camera;
    std::int64_t slot = 0;
    std::uint32_t received = 0, consumed = 0, generation = 0;
    std::uint32_t playing = 0;
    std::string build, applied;
    std::deque<proto::LilyFrame> frames;
    std::deque<Step> pending;
    float elapsed = 0, rate = 1;
    bool started = false;
    bool ready = false;
    float world_rate = 1;
};
std::map<std::uint16_t, Replica> g_replicas;
std::uint64_t g_captured = 0, g_received = 0, g_consumed = 0, g_profiles = 0, g_state_changes = 0;
std::uint64_t g_edges_received = 0, g_edges_played = 0, g_starved = 0;
auto& h_tick = metrics::histogram("native_lily.tick_us");
auto& h_rate = metrics::histogram("native_lily.component_rate");
auto& h_steps = metrics::histogram("native_lily.steps");
auto& h_queue = metrics::histogram("native_lily.buffer_ms");
auto& h_correct = metrics::histogram("native_lily.correction_uu");
auto& c_paused = metrics::counter("native_lily.paused_frames");
auto& c_hitch = metrics::counter("native_lily.host_hitches_compressed");
void fail(std::string why) { if (g_fault.empty()) g_fault = std::move(why); }
bool resolve() {
    if (g_schema.ready()) return true;
    Schema s;
    auto* a = reflect::find_class(L"/Script/Engine.Actor");
    auto* z = reflect::find_class(L"/Script/Zenith.ZenithCharacter");
    auto* m = reflect::find_class(L"/Script/Engine.CharacterMovementComponent");
    auto* st = reflect::find_class(L"/Script/Zenith.StateComponent");
    reflect::Resolver ar("NativeLily Actor", a), zr("NativeLily Character", z), mr("NativeLily Movement", m), sr("NativeLily State", st);
    s.dilation = ar.prop(L"CustomTimeDilation", reflect::Kind::Float, 4);
    s.get_owner = ar.func(L"GetOwner", 0);
    s.state_component = zr.prop(L"StateComponent", reflect::Kind::Object);
    s.velocity = mr.prop(L"Velocity", reflect::Kind::Struct, 12);
    s.custom_mode = mr.prop(L"CustomMovementMode", reflect::Kind::Any, 1);
    s.states = sr.prop(L"ActiveStates", reflect::Kind::Array);
    s.add_state = sr.func(L"AddState", 1);
    s.remove_state = sr.func(L"RemoveState", 1);
    reflect::Resolver pr("NativeLily Pawn", reflect::find_class(L"/Script/Engine.Pawn"));
    s.get_controller = pr.func(L"GetController", 0);
    auto* ai = reflect::find_class(L"/Script/Zenith.ZenithAICompanionController");
    reflect::Resolver cr("NativeLily Companion leader", ai);
    s.leader = cr.func(L"GetLeaderPawn", 0); s.companion_ai = ObjHandle::of(ai);
    reflect::Resolver gr("NativeLily World clock", reflect::find_class(L"/Script/Engine.GameplayStatics"));
    s.world_delta = gr.func(L"GetWorldDeltaSeconds", 1);
    s.gameplay = ObjHandle::of(reflect::find_object(L"/Script/Engine.Default__GameplayStatics"));
    if (!ar.ok() || !zr.ok() || !mr.ok() || !sr.ok() || !pr.ok() || !cr.ok() || !gr.ok() || !s.gameplay.valid() || reflect::element_size(s.states) != 8) {
        fail(ar.failure() + zr.failure() + mr.failure() + sr.failure() + pr.failure() + cr.failure() + gr.failure());
        return false;
    }
    s.actor = ObjHandle::of(a); s.character = ObjHandle::of(z); s.movement = ObjHandle::of(m); s.state = ObjHandle::of(st);
    g_schema = s;
    return true;
}
float world_delta(ue::UObject* pawn) {
    reflect::Frame clock(g_schema.world_delta);
    if (!clock.arg(0, pawn).invoke(g_schema.gameplay.get())) { fail("native Lily World clock unavailable"); return 0; }
    return clock.ret<float>();
}
std::uint16_t seat_of(ue::UObject* pawn) {
    if (!pawn) return 0;
    if (pawn == world::p1_pawn()) return 1;
    const auto slot = core::slot_of(pawn);
    return slot > 1 && slot <= 8 ? static_cast<std::uint16_t>(slot) : 0;
}
void consumed(ue::UObject* pawn, const input::InputSnapshot& value) {
    if (!g_sink || !g_name || !g_fault.empty()) return;
    const auto seat = seat_of(pawn);
    const auto* s = core::schema::script();
    if (!seat || !s || !resolve()) return;
    auto* movement = s->char_movement.object(pawn);
    auto* state = g_schema.state_component.object(pawn);
    auto at = core::location(pawn);
    if (!movement || !state || !at) return fail("native Lily capture has no movement/state/position");
    if (g_sequence[seat] == ~std::uint32_t{0}) return fail("native Lily input sequence exhausted");
    proto::LilyFrame f;
    f.seat = seat; f.seq = ++g_sequence[seat]; f.dt = g_source_dt; f.step = world_delta(pawn);
    f.t_ms = static_cast<std::uint32_t>(clock::now_us()/1000); f.world_dt = f.step;
    f.generation = static_cast<std::uint32_t>(ObjHandle::of(pawn).serial);
    if (!std::isfinite(f.step) || f.step <= 0 || f.step > 60) return fail(std::format("native Lily Host World delta out of range: {}", f.step));
    const float interval = g_source_cadence[seat].interval(f.generation, f.step);
    if (interval < f.step) ++c_hitch;
    f.step = interval; // retain f.dt in full: do not replay a Host stall as a second delayed pause
    std::memcpy(f.input.data(), &value, sizeof(float) * 7);
    f.direction = value.direction;
    f.flags = static_cast<std::uint16_t>(value.flags & 0x3FEu);
    f.raw = static_cast<std::uint16_t>(value.raw & 0x3FEu);
    f.position = {at->x, at->y, at->z};
    std::memcpy(f.velocity.data(), g_schema.velocity.at<void>(movement), 12);
    f.mode = s->cm_mode.read<std::uint8_t>(movement); f.custom_mode = g_schema.custom_mode.read<std::uint8_t>(movement);
    const auto arr = reflect::array_at(state, g_schema.states);
    if (arr.num < 0 || arr.num > static_cast<int>(proto::kLilyStates) || (arr.num && !arr.data)) return fail("native Lily state capacity/layout");
    for (int i = 0; i < arr.num; ++i) {
        auto* cls = reinterpret_cast<ue::UObject**>(arr.data)[i];
        if (!ue::alive(cls)) return fail("native Lily state class expired");
        const auto id = g_name(log::utf8(cls->GetPathName()));
        if (!id) return fail("native Lily state name capacity");
        f.states[f.state_count++] = id;
    }
    ++g_captured;
    g_sink(pawn, f);
}
bool apply_build(Replica& r) {
    if (r.build.empty()) return false;
    if (r.applied == r.build) return true;
    auto* pawn = r.pawn.get();
    const auto* s = core::schema::script();
    auto* pc = pawn && s ? s->pawn_controller.object(pawn) : nullptr;
    if (!pc) return false;
    std::string why;
    json::Value j;
    core::profile::Profile p;
    if (!json::parse(r.build, j, &why) || !core::profile::from_json(j, p, &why)) { fail("native Lily profile: " + why); return false; }
    const auto result = core::profile::reset(pc, p);
    if (!result.ok()) { fail("native Lily profile apply: " + result.text()); return false; }
    r.applied = r.build; ++g_profiles;
    return true;
}
}
namespace { bool install_tick(); }
bool host_enable(FrameSink sink, Name name) {
    if (!resolve()) return false;
    g_sink = sink; g_name = name;
    return install_tick() && input::pad::observe(consumed) && causality::enable();
}
void host_disable() { input::pad::observe(nullptr); g_sink = nullptr; g_name = nullptr; g_sequence.fill(0); g_source_cadence = {}; g_fault.clear(); causality::reset(); }
namespace { bool install_tick(); }
std::string bind(std::uint16_t seat, std::int64_t slot, ue::UObject* pawn) {
    if (!pawn || !resolve()) return g_fault.empty() ? "native Lily pawn missing" : g_fault;
    if (!install_tick()) return "native Lily original Tick preflight failed";
    if (!ability::client_native_enable()) return "native Lily ability lifecycle hooks unavailable";
    if (!causality::enable()) return "native Lily causality hooks unavailable";
    auto& r = g_replicas[seat];
    const auto h = ObjHandle::of(pawn);
    if (r.pawn == h) return {};
    r.pawn = h; r.slot = slot; r.applied.clear(); r.started = false; r.ready = false; r.elapsed = 0; r.rate = 1; r.pending.clear();
    const auto* s = core::schema::script();
    if (!s) return "native Lily core schema";
    r.controller = ObjHandle::of(s->pawn_controller.object(pawn));
    reflect::Resolver pc_schema("native camera owner", reflect::find_class(L"/Script/Engine.PlayerController"));
    reflect::Resolver manager_schema("native camera manager", reflect::find_class(L"/Script/Zenith.PlayerCameraManagerZenithBase"));
    const auto manager_prop = pc_schema.prop(L"PlayerCameraManager", reflect::Kind::Object);
    const auto registered = manager_schema.prop(L"RegisteredGameplayCamera", reflect::Kind::Weak, 8);
    if (!pc_schema.ok() || !manager_schema.ok()) return "native Lily camera owner schema";
    auto* manager = manager_prop.object(world::p1_controller());
    auto* camera = manager ? registered.at<RC::Unreal::FWeakObjectPtr>(manager)->Get() : nullptr;
    if (!ue::alive(camera)) return "native Lily original camera unavailable";
    auto reg = reflect::Resolver("native camera", camera->GetClassPrivate()).func(L"RegisterAllyController", 1);
    if (!reg.ok() || !reflect::Frame(reg).arg(0, r.controller.get()).invoke(camera)) return "native Lily original camera registration failed";
    r.camera = ObjHandle::of(camera);
    // Keep world blocking for the original CharacterMovement, remove world-trigger/contact overlap.
    reflect::Frame parts(s->actor_components_by_class); parts.arg(0, s->primitive_class.get());
    if (!parts.invoke(pawn)) return "native Lily primitive enumeration";
    const auto arr = parts.ret<reflect::RawArray>();
    if (arr.num < 0 || arr.num > 64 || (arr.num && !arr.data)) return "native Lily primitive layout";
    for (int i = 0; i < arr.num; ++i) {
        auto* component = reinterpret_cast<ue::UObject**>(arr.data)[i];
        if (ue::alive(component)) reflect::Frame(s->prim_set_overlaps).arg_bool(0, false).invoke(component);
    }
    guard::unmark(pawn);
    guard::mark_not_player(pawn, true);
    *g_schema.dilation.at<float>(pawn) = 0;
    return {};
}
void build(std::uint16_t seat, const std::string& value) { if (seat && seat <= 8) g_replicas[seat].build = value; else fail("native Lily build seat"); }
void receive(const proto::LilyFrame& value) {
    if (auto it = g_replicas.find(value.seat); it != g_replicas.end() && it->second.generation && it->second.generation != value.generation && it->second.pawn.valid()) {
        const auto slot = it->second.slot;
        const auto build_value = it->second.build;
        const int skin = core::skin::current(it->second.pawn.get());
        forget(value.seat);
        core::spawn::Options options;
        options.location = core::Vec3{value.position[0], value.position[1], value.position[2]};
        options.skin = core::skin::valid(skin) ? skin : 1;
        if (const auto why = core::spawn::respawn(slot, options); !why.empty()) return fail("native Lily Pawn replacement: " + why);
        if (const auto why = bind(value.seat, slot, core::pawn(slot)); !why.empty()) return fail(why);
        g_replicas[value.seat].build = build_value;
    }
    auto& r = g_replicas[value.seat];
    r.generation = value.generation;
    if (value.seq <= r.received) return;
    if (r.frames.size() >= 120) return fail(std::format("native Lily reliable input backlog: pawn={} build={} paused={} received={} consumed={} pending={}", r.pawn.valid(), !r.build.empty(), world::paused(), r.received, r.consumed, r.pending.size()));
    r.received = value.seq; r.frames.push_back(value); ++g_received;
    g_edges_received += std::popcount(static_cast<unsigned>(value.flags & 0x1FEu));
}
namespace {
void execute_frame(Replica& r, const Replica::Step& step) {
    const auto& f = step.frame;
    auto* pawn = r.pawn.get();
    const auto* s = core::schema::script();
    if (!pawn || !s || !g_text) return fail("native Lily execution references expired");
    auto* movement = s->char_movement.object(pawn);
    auto* state = g_schema.state_component.object(pawn);
    if (!movement || !state) { fail("native Lily execution context missing"); return; }
    if (step.begin) {
        std::vector<ue::UObject*> wanted;
        for (std::size_t i = 0; i < f.state_count; ++i) {
            const auto* path = g_text(f.states[i]);
            auto& cached = g_state_classes[f.states[i]];
            auto* cls = cached.get();
            if (!cls && path) {
                cls = reflect::load_object(log::wide(*path));
                if (cls) cached = ObjHandle::of(cls);
            }
            if (!cls) { fail("native Lily authoritative state unavailable"); return; }
            wanted.push_back(cls);
        }
        const auto arr = reflect::array_at(state, g_schema.states);
        if (arr.num < 0 || arr.num > 128 || (arr.num && !arr.data)) { fail("native Lily local state layout"); return; }
        std::vector<ue::UObject*> previous;
        for (int i = 0; i < arr.num; ++i) previous.push_back(reinterpret_cast<ue::UObject**>(arr.data)[i]);
        for (auto* cls : previous) if (std::find(wanted.begin(), wanted.end(), cls) == wanted.end()) { reflect::Frame(g_schema.remove_state).arg(0, cls).invoke(state); ++g_state_changes; }
        for (auto* cls : wanted) if (std::find(previous.begin(), previous.end(), cls) == previous.end()) { reflect::Frame(g_schema.add_state).arg(0, cls).invoke(state); ++g_state_changes; }
        if (s->cm_mode.read<std::uint8_t>(movement) != f.mode || g_schema.custom_mode.read<std::uint8_t>(movement) != f.custom_mode)
            reflect::Frame(s->cm_set_mode).arg(0, f.mode).arg(1, f.custom_mode).invoke(movement);
    }
    input::InputSnapshot input;
    std::memcpy(&input, f.input.data(), sizeof(float) * 7);
    input.direction = f.direction;
    input.flags = step.begin ? f.flags & ~(1u << 9) : 0;
    input.raw = f.raw & ~(1u << 9); // no local world interaction; no repeated press on partial frames
    if (const auto why = input::pad::snapshot(r.slot, input); !why.empty()) { fail(why); return; }

    if (!g_fault.empty()) return;
    r.playing = f.seq;
    ExecutionScope source_scope(pawn);
    g_original_tick(pawn, f.dt);
    if (step.begin) g_edges_played += std::popcount(static_cast<unsigned>(input.flags));
    if (step.end) { r.consumed = f.seq; ++g_consumed; }
}
float buffered(const Replica& r) {
    float total = -r.elapsed;
    for (const auto& f : r.frames) total += f.step;
    return std::max(total, 0.0f);
}
void anchor(Replica& r) {
    const auto* s = core::schema::script();
    auto* pawn = r.pawn.get();
    if (!s || !pawn || r.frames.empty()) return;
    const auto& a = r.frames.front();
    const auto& b = r.frames.size() > 1 ? r.frames[1] : a;
    // This is movement context at the start of this component step, not one teleport for each packet.
    const float t = std::clamp(r.elapsed / a.step, 0.0f, 1.0f);
    const auto coord = [&](std::size_t i) { return a.position[i] + (b.position[i] - a.position[i]) * t; };
    core::Vec3 at{coord(0), coord(1), coord(2)};
    if (auto current = core::location(pawn)) {
        const float error = std::hypot(current->y - at.y, current->z - at.z);
        h_correct.add(error);
        if (error > 0.05f) {
            const float blend = !r.started || error > 250.0f ? 1.0f : 1.0f - std::exp(-20.0f * g_dt);
            at = {current->x + (at.x - current->x) * blend, current->y + (at.y - current->y) * blend, current->z + (at.z - current->z) * blend};
            reflect::Frame(s->actor_teleport).arg(0, at).arg_bool(1, false).arg_bool(3, false).invoke(pawn);
        }
    }
    if (auto* movement = s->char_movement.object(pawn)) {
        auto* velocity = g_schema.velocity.at<float>(movement);
        for (std::size_t i = 0; i < 3; ++i) velocity[i] = a.velocity[i] + (b.velocity[i] - a.velocity[i]) * t;
    }
}
void schedule(Replica& r);
void __fastcall character_tick(ue::UObject* pawn, float dt) {
    if (!g_sink && g_replicas.empty()) { g_original_tick(pawn, dt); return; }
    for (auto& [seat, r] : g_replicas) {
        if (r.pawn.get() != pawn) continue;
        const auto begin = clock::now_us();
        g_dt = world_delta(pawn);
        if (!std::isfinite(g_dt) || g_dt <= 0 || !r.ready || !g_fault.empty()) {
            *g_schema.dilation.at<float>(pawn) = 0;
            return;
        }
        schedule(r);
        if(r.pending.empty() && r.playing)causality::advance(causality::playback_ms(),0);
        h_steps.add(static_cast<float>(r.pending.size()));
        while (!r.pending.empty() && g_fault.empty()) {
            const auto value = r.pending.front(); r.pending.pop_front();
            causality::advance(value.host_ms, r.world_rate);
            execute_frame(r, value);
        }
        h_tick.add(static_cast<float>(clock::now_us() - begin));
        return;
    }
    const auto previous = g_source_dt;
    g_source_dt = dt;
    ExecutionScope source_scope(pawn);
    g_original_tick(pawn, dt);
    g_source_dt = previous;
}
bool install_tick() {
    return g_original_tick || hook::install_manifest("NativeLilyCharacterTick", reinterpret_cast<void*>(&character_tick), reinterpret_cast<void**>(&g_original_tick));
}
}
namespace {
void schedule(Replica& r) {
    auto* pawn = r.pawn.get();
    *g_schema.dilation.at<float>(pawn) = 0;
    if (!r.pending.empty()) {
        float total = 0;
        for (const auto& part : r.pending) total += part.frame.dt;
        *g_schema.dilation.at<float>(pawn) = total / std::max(g_dt, 0.001f);
        return;
    }
    const float buffer = buffered(r);
    h_queue.add(buffer * 1000);
    if (r.frames.empty() || (!r.started && buffer < 0.05f)) {
        if (r.started) { ++g_starved; h_rate.add(0); }
        return;
    }
    anchor(r);
    r.started = true;
    // Correct only clock drift, never multiply time by the integer count of arrivals.
    const float desired = std::clamp(1.0f + (buffer - 0.05f) * 2.0f, 0.9f, 1.1f);
    r.rate += (desired - r.rate) * (1.0f - std::exp(-4.0f * g_dt));
    float budget = g_dt * r.rate, total = 0, world_total = 0;
    while (budget > 0 && !r.frames.empty()) {
        const auto& source = r.frames.front();
        const float remaining = std::max(source.step - r.elapsed, 0.0f);
        const float span = std::min(remaining, budget);
        Replica::Step part{source, r.elapsed == 0, remaining <= budget};
        const auto until = r.frames.size() > 1 ? static_cast<std::int32_t>(r.frames[1].t_ms-source.t_ms) : source.step*1000;
        part.host_ms = source.t_ms + std::max(0.0,static_cast<double>(until)) * ((r.elapsed+span)/source.step);
        if(!causality::ready(part.host_ms))break;
        part.frame.dt = source.dt * (span / source.step);
        world_total += source.world_dt * (span/source.step);
        total += part.frame.dt;
        r.pending.push_back(part);
        budget -= span;
        if (part.end) { r.frames.pop_front(); r.elapsed = 0; }
        else { r.elapsed += span; break; }
    }
    if (budget > 0.0001f) ++g_starved;
    *g_schema.dilation.at<float>(pawn) = total / std::max(g_dt, 0.001f);
    r.world_rate = world_total / std::max(g_dt,0.001f);
    h_rate.add(*g_schema.dilation.at<float>(pawn));
}
}
void client_tick(std::uint16_t map, Text text) {
    if (!g_fault.empty() || !resolve()) return;
    if (world::paused()) { ++c_paused; return; }
    g_text = text;
    for (auto& [seat, r] : g_replicas) {
        if (!r.pawn.valid()) continue;
        while (!r.frames.empty() && r.frames.front().map != map) { r.frames.pop_front(); r.elapsed = 0; }
        r.ready = apply_build(r);
    }
}
void forget(std::uint16_t seat) {
    if (auto it = g_replicas.find(seat); it != g_replicas.end()) {
        auto* camera = it->second.camera.get(); auto* pc = it->second.controller.get();
        if (camera && pc) {
            const auto fn = reflect::Resolver("native camera", camera->GetClassPrivate()).func(L"UnregisterAllyController", 1);
            if (fn.ok()) reflect::Frame(fn).arg(0, pc).invoke(camera);
        }
        g_replicas.erase(it);
    }
}
void reset() { g_replicas.clear(); g_state_classes.clear(); g_text = nullptr; g_fault.clear(); causality::reset(); }
ue::UObject* actor(std::uint16_t seat) { const auto i = g_replicas.find(seat); return i == g_replicas.end() ? nullptr : i->second.pawn.get(); }
bool owns(ue::UObject* object) {
    if (!object || g_replicas.empty() || !g_schema.ready()) return false;
    const auto* s = core::schema::script();
    for (int depth = 0; object && depth < 8; ++depth) {
        for (const auto& [seat, r] : g_replicas) {
            auto* pawn = r.pawn.get();
            auto* pc = pawn && s ? s->pawn_controller.object(pawn) : nullptr;
            if (object == pawn || (pc && object == pc)) return true;
        }
        auto* cls = static_cast<ue::UClass*>(g_schema.actor.get());
        if (cls && object->IsA(cls)) {
            if (auto* ai = static_cast<ue::UClass*>(g_schema.companion_ai.get()); ai && object->IsA(ai)) {
                reflect::Frame leader(g_schema.leader);
                if (!leader.invoke(object)) return false;
                object = leader.ret<ue::UObject*>();
                continue;
            }
            reflect::Frame owner(g_schema.get_owner);
            if (!owner.invoke(object)) return false;
            object = owner.ret<ue::UObject*>();
        } else object = object->GetOuterPrivate();
    }
    return false;
}
std::string fault() { return g_fault; }
bool source(ue::UObject* object, Source& out) {
    out = {};
    if (!object || !g_schema.ready()) return false;
    const auto* s = core::schema::script();
    for (int depth = 0; object && depth < 8; ++depth) {
        if (g_sink) {
            for (std::uint16_t seat = 1; seat <= 8; ++seat) {
                auto* pawn = seat == 1 ? world::p1_pawn() : core::pawn(seat);
                auto* pc = seat == 1 ? world::p1_controller() : core::controller(seat);
                if (object == pawn || (pc && object == pc)) { out = {seat, static_cast<std::uint32_t>(ObjHandle::of(pawn).serial), g_sequence[seat],g_executing_pawn.get()==pawn}; return true; }
            }
        } else for (const auto& [seat, r] : g_replicas) {
            if (object == r.pawn.get() || object == r.controller.get()) { out = {seat, r.generation, r.playing,g_executing_pawn==r.pawn}; return true; }
        }
        if (auto* cls = static_cast<ue::UClass*>(g_schema.actor.get()); cls && object->IsA(cls)) {
            if (auto* ai = static_cast<ue::UClass*>(g_schema.companion_ai.get()); ai && object->IsA(ai)) {
                reflect::Frame leader(g_schema.leader); object = leader.invoke(object) ? leader.ret<ue::UObject*>() : nullptr;
            } else { reflect::Frame owner(g_schema.get_owner); object = owner.invoke(object) ? owner.ret<ue::UObject*>() : nullptr; }
        } else object = object->GetOuterPrivate();
    }
    return false;
}
std::string describe() {
    std::size_t queued = 0; for (const auto& [seat, r] : g_replicas) queued += r.frames.size();
    return std::format("native Lily: replicas={} captured={} received={} consumed={} queued={} profiles={} stateChanges={} edges={}/{} starved={} fault='{}'\n", g_replicas.size(), g_captured, g_received, g_consumed, queued, g_profiles, g_state_changes, g_edges_played, g_edges_received, g_starved, g_fault);
}
}
