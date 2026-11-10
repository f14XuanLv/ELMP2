#include "coop/native_lily.h"
#include "coop/session.h"
#include "coop/ability.h"
#include "coop/causality.h"
#include "coop/command_context.h"
#include "coop/lily_events.h"
#include "coop/autonomous.h"
#include "core/permission.h"
#include "core/player.h"
#include "core/profile.h"
#include "core/schema.h"
#include "core/spawn.h"
#include "core/skin.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
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
    reflect::FuncRef leader, set_facing;
    reflect::PropRef aptitude_changed;
    ObjHandle gameplay;
    reflect::FuncRef world_delta;
    bool ready() const { return actor.valid() && character.valid() && movement.valid() && state.valid() && gameplay.valid() && world_delta.ok(); }
};
Schema g_schema;
std::string g_fault;
bool g_playback_gap = false;
float g_dt = 1.0f / 60.0f;
float g_playout_dt = 1.0f / 60.0f;
float g_source_dt = 1.0f / 60.0f;
Text g_text = nullptr;
using CharacterTick = void(__fastcall*)(ue::UObject*, float);
CharacterTick g_original_tick = nullptr;
std::unordered_map<std::uint16_t, ObjHandle> g_state_classes;
FrameSink g_sink = nullptr;
Name g_name = nullptr;
std::array<std::uint32_t, 9> g_sequence{};
ObjHandle g_executing_pawn;
ObjHandle g_restore_pawn;
Source g_restore_source;
struct RestoreScope {
    ObjHandle previous_pawn=g_restore_pawn;Source previous_source=g_restore_source;
    RestoreScope(ue::UObject* pawn,Source origin){g_restore_pawn=ObjHandle::of(pawn);g_restore_source=origin;}
    ~RestoreScope(){g_restore_pawn=previous_pawn;g_restore_source=previous_source;}
};
struct ExecutionScope { ObjHandle previous=g_executing_pawn; explicit ExecutionScope(ue::UObject* pawn){g_executing_pawn=ObjHandle::of(pawn);}~ExecutionScope(){g_executing_pawn=previous;} };
struct SourceCadence {
    std::uint32_t generation = 0;
    std::int64_t last_us = 0;
    float physical = 1.0f / 60;
    std::array<float, 31> samples{};
    std::size_t next = 0;
    float interval(std::uint32_t serial, std::int64_t now) {
        if (generation != serial) { generation = serial; samples.fill(1.0f / 60); next = 0; last_us = 0; }
        physical = last_us ? static_cast<float>(now-last_us)/1'000'000 : 1.0f/60;
        last_us = now;
        if (physical <= 0) physical = 1.0f/60;
        samples[next++ % samples.size()] = physical;
        auto sorted = samples;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        return std::min(physical, sorted[sorted.size() / 2] * 1.5f);
    }
};
std::array<SourceCadence, 9> g_source_cadence{};
struct Replica {
    struct Step { proto::LilyFrame frame; bool begin = false, end = false; double host_ms = 0; };
    ObjHandle pawn, controller;
    std::int64_t slot = 0;
    std::uint32_t received = 0, consumed = 0, generation = 0;
    std::uint32_t playing = 0;
    std::uint64_t current_host = 0, current_local = 0;
    std::uint64_t persistent_host = 0, persistent_local = 0;
    bool persistent_restored = false;
    std::int64_t tick_us = 0;
    proto::LilyFrame last;
    std::string build, applied;
    std::deque<proto::LilyFrame> frames;
    std::deque<Step> pending;
    float elapsed = 0, rate = 1;
    bool started = false;
    bool ready = false;
    float world_rate = 1;
};
struct Capture {
    ObjHandle pawn;
    std::optional<proto::LilyFrame> frame;
    std::vector<proto::LilyCommand> commands;
};
std::array<Capture, 9> g_capture;
std::uint64_t g_command_starts = 0, g_command_ends = 0;
std::map<std::uint16_t, Replica> g_replicas;
struct RoleClock { ObjHandle pawn; float original = 1, applied = 1; };
std::unordered_map<ObjHandle, RoleClock> g_role_clocks;
std::uint64_t g_clock_writes = 0;
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
    s.aptitude_changed=zr.prop(L"OnAptitudeUnlocked",reflect::Kind::Delegate,sizeof(RC::Unreal::FMulticastScriptDelegate));
    s.get_owner = ar.func(L"GetOwner", 0);
    s.set_facing = reflect::Resolver("native facing", reflect::find_class(L"/Script/Zenith.LocomotionComponent")).func(L"SetFacing", 1);
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
    if (!ar.ok() || !zr.ok() || !mr.ok() || !sr.ok() || !pr.ok() || !cr.ok() || !gr.ok() || !s.set_facing.ok() || !s.gameplay.valid() || reflect::element_size(s.states) != 8) {
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
std::vector<ue::UObject*> owned_actors(std::uint16_t seat) {
    std::vector<ue::UObject*> out;
    for(auto kind:{registry::Kind::Spirit,registry::Kind::Companion}) for(const auto& entry:registry::bucket(kind))
        if(auto* actor=entry.handle.get();actor&&!autonomous::eligible(actor)){Source origin;if(source(actor,origin)&&origin.seat==seat)out.push_back(actor);}
    return out;
}
void restore_clocks(ue::UObject* pawn = nullptr) {
    const auto owner=ObjHandle::of(pawn);
    std::erase_if(g_role_clocks,[&](const auto& p){
        if(pawn && p.second.pawn!=owner)return false;
        if(auto* actor=p.first.get())*g_schema.dilation.at<float>(actor)=p.second.original;
        return true;
    });
}
void apply_clocks(Replica& r,const proto::LilyFrame& f) {
    std::vector<std::pair<ue::UObject*,float>> types;
    for(unsigned i=0;i<f.actor_clock_count;++i){
        const auto& clock=f.actor_clocks[i];auto& weak=g_state_classes[clock.cls];auto* cls=weak.get();
        if(!cls){const auto* name=g_text(clock.cls);if(name)cls=reflect::load_object(log::wide(*name));if(cls)weak=ObjHandle::of(cls);}
        if(!cls){fail("native owned actor clock class unavailable");return;}
        types.emplace_back(cls,clock.scale);
    }
    for(auto* actor:owned_actors(f.seat)){
        const auto type=std::find_if(types.begin(),types.end(),[&](const auto& t){return actor->GetClassPrivate()==t.first;});
        if(type==types.end())continue; // equipment baselines may be in flight; never apply another class's clock
        const auto h=ObjHandle::of(actor);auto it=g_role_clocks.find(h);
        if(it==g_role_clocks.end())it=g_role_clocks.emplace(h,RoleClock{r.pawn,g_schema.dilation.read<float>(actor),1}).first;
        if(it->second.pawn!=r.pawn){*g_schema.dilation.at<float>(actor)=it->second.original;it->second.pawn=r.pawn;}
        const float value=r.world_rate*type->second;
        if(g_schema.dilation.read<float>(actor)!=value){*g_schema.dilation.at<float>(actor)=value;++g_clock_writes;}
        it->second.applied=value;
    }
    std::erase_if(g_role_clocks,[](const auto& e){
        auto* owned=e.first.get();if(!owned)return true;
        Source origin;
        if(source(owned,origin)&&ObjHandle::of(native_lily::actor(origin.seat))==e.second.pawn)return false;
        *g_schema.dilation.at<float>(owned)=e.second.original;return true;
    });
}
void consumed(ue::UObject* pawn, const input::InputSnapshot& value) {
    if (autonomous::consume_input(pawn)) return;
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
    const auto sampled_at=clock::now_us();
    f.t_ms = static_cast<std::uint32_t>(sampled_at/1000); f.world_dt = f.step;
    f.generation = static_cast<std::uint32_t>(ObjHandle::of(pawn).serial);
    if (!std::isfinite(f.step) || f.step <= 0 || f.step > 60) return fail(std::format("native Lily Host World delta out of range: {}", f.step));
    const float interval = g_source_cadence[seat].interval(f.generation, sampled_at);
    if (interval < g_source_cadence[seat].physical) ++c_hitch;
    f.step = interval; // retain f.dt in full: do not replay a Host stall as a second delayed pause
    std::memcpy(f.input.data(), &value, sizeof(float) * 7);
    f.direction = value.direction;
    f.flags = static_cast<std::uint16_t>(value.flags & 0x3FEu);
    f.raw = static_cast<std::uint16_t>(value.raw & 0x3FEu);
    f.position = {at->x, at->y, at->z};
    std::memcpy(f.velocity.data(), g_schema.velocity.at<void>(movement), 12);
    f.mode = s->cm_mode.read<std::uint8_t>(movement); f.custom_mode = g_schema.custom_mode.read<std::uint8_t>(movement);
    reflect::Frame facing(s->loco_facing_right);
    if(!facing.invoke(s->zc_locomotion.object(pawn)))return fail("native facing capture unavailable");
    f.facing = facing.ret_bool()?2:1;
    const auto arr = reflect::array_at(state, g_schema.states);
    if (arr.num < 0 || arr.num > static_cast<int>(proto::kLilyStates) || (arr.num && !arr.data)) return fail("native Lily state capacity/layout");
    for (int i = 0; i < arr.num; ++i) {
        auto* cls = reinterpret_cast<ue::UObject**>(arr.data)[i];
        if (!ue::alive(cls)) return fail("native Lily state class expired");
        const auto id = g_name(log::utf8(cls->GetPathName()));
        if (!id) return fail("native Lily state name capacity");
        f.states[f.state_count++] = id;
    }
    for (auto* cls : command_context::owned_states(pawn)) {
        if(f.command_state_count==proto::kLilyStates)return fail("native command state capacity");
        const auto id=g_name(log::utf8(cls->GetPathName()));if(!id)return fail("native command state name capacity");
        f.command_states[f.command_state_count++]=id;
    }
    if(const auto why=command_context::fault();!why.empty())return fail(why);
    if(const auto p=command_context::persistent(pawn);p.cls&&p.input){
        f.persistent_class=g_name(log::utf8(p.cls->GetPathName()));f.persistent_token=p.token;f.persistent_input=p.input;
        f.persistent_direction=p.direction;f.persistent_button=p.button;f.persistent_elapsed=p.elapsed;
    }
    for(auto* actor:owned_actors(seat)) {
        const auto id=g_name(log::utf8(actor->GetClassPrivate()->GetPathName()));
        const float scale=g_schema.dilation.read<float>(actor);
        auto begin=f.actor_clocks.begin(),end=begin+f.actor_clock_count;
        const auto it=std::find_if(begin,end,[&](const auto& c){return c.cls==id;});
        if(it!=end){if(std::abs(it->scale-scale)>.0001f)return fail("owned actors of one class have conflicting clocks");continue;}
        if(f.actor_clock_count==proto::kLilyClocks)return fail("native owned actor clock capacity");
        f.actor_clocks[f.actor_clock_count++]={id,scale};
    }
    ++g_captured;
    auto& capture = g_capture[seat];
    const auto handle = ObjHandle::of(pawn);
    if (capture.pawn != handle) capture = {handle};
    capture.frame = std::move(f);
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
    // The PC's inventory can survive Pawn replacement. Re-publish its capability context to the
    // new Pawn's native listeners even when adding the same inventory rows was a no-op.
    auto* listeners=static_cast<RC::Unreal::FMulticastDelegateProperty*>(g_schema.aptitude_changed.prop)->GetMulticastDelegate(g_schema.aptitude_changed.at<void>(pawn));
    if(!listeners){fail("native aptitude notification unavailable");return false;}
    // Snapshot the original delegate endpoints: callbacks may mutate the invocation list.
    // Use our counted reflection frame instead of the SDK's inline-allocator delegate copy.
    std::vector<RC::Unreal::FScriptDelegate> endpoints;
    for(const auto& item:listeners->InvocationList)endpoints.push_back(item);
    for(const auto& item:endpoints)if(auto* object=item.GetUObject()){
        reflect::Resolver listener("native capability listener",object->GetClassPrivate());
        const auto callback=listener.func(item.GetFunctionName().ToString(),0);
        if(!listener.ok()||!reflect::Frame(callback).invoke(object)){fail("native aptitude listener failed");return false;}
    }
    r.applied = r.build; ++g_profiles;
    return true;
}
}
namespace { bool install_tick(); }
bool host_enable(FrameSink sink, Name name) {
    if (!resolve()) return false;
    g_sink = sink; g_name = name;
    return install_tick() && input::pad::observe(consumed) && causality::enable() && command_context::enable();
}
void host_disable() { input::pad::observe(nullptr); g_sink = nullptr; g_name = nullptr; g_sequence.fill(0); g_capture = {}; g_source_cadence = {}; g_fault.clear(); restore_clocks(); command_context::reset(); causality::reset(); }
namespace { bool install_tick(); }
std::string bind(std::uint16_t seat, std::int64_t slot, ue::UObject* pawn) {
    if (!pawn || !resolve()) return g_fault.empty() ? "native Lily pawn missing" : g_fault;
    if (!install_tick() || !input::pad::observe(consumed)) return "native Lily original Tick/input preflight failed";
    if (!ability::client_native_enable()) return "native Lily ability lifecycle hooks unavailable";
    if (!causality::enable()) return "native Lily causality hooks unavailable";
    if (!command_context::enable()) return "native command ownership hooks unavailable";
    auto& r = g_replicas[seat];
    const auto h = ObjHandle::of(pawn);
    if (r.pawn == h) return {};
    r.pawn = h; r.slot = slot; r.applied.clear(); r.started = false; r.ready = false; r.elapsed = 0; r.rate = 1; r.playing = 0; r.pending.clear();
    r.current_host=r.current_local=0; r.persistent_host=r.persistent_local=0;r.persistent_restored=false;
    const auto* s = core::schema::script();
    if (!s) return "native Lily core schema";
    r.controller = ObjHandle::of(s->pawn_controller.object(pawn));
    // Remote execution has no camera ownership: Client keeps vanilla P1-only framing (F2.7).
    // Preserve native environment sensors and physics-volume perception. Authority is restricted at
    // the existing target/collection and settlement gates, rather than disabling every overlap source.
    for (unsigned i=0;i<static_cast<unsigned>(core::permission::Gate::Count);++i)
        core::permission::set(slot,static_cast<core::permission::Gate>(i),false);
    core::permission::track(slot);
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
    if (r.frames.size() >= 120) {
        if (g_fault.empty()) g_playback_gap = true;
        return fail(std::format("Lily input backlog seat={} generation={} pawn={} ready={} paused={} received={} consumed={} pending={}",
                                value.seat, value.generation, r.pawn.valid(), r.ready, world::paused(), r.received, r.consumed, r.pending.size()));
    }
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
        auto* loco=s->zc_locomotion.object(pawn);reflect::Frame facing(s->loco_facing_right);
        if(!loco||!facing.invoke(loco)){fail("native facing playback unavailable");return;}
        if(facing.ret_bool()!=(f.facing==2))reflect::Frame(g_schema.set_facing).arg_bool(0,f.facing==2).invoke(loco);
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
        auto protected_states = command_context::owned_states(pawn);
        for(unsigned i=0;i<f.command_state_count;++i) {
            const auto* path=g_text(f.command_states[i]);auto& cached=g_state_classes[f.command_states[i]];
            auto* cls=cached.get();if(!cls&&path){cls=reflect::load_object(log::wide(*path));if(cls)cached=ObjHandle::of(cls);}
            if(!cls){fail("native command state name unavailable");return;}
            if(std::find(protected_states.begin(),protected_states.end(),cls)==protected_states.end())protected_states.push_back(cls);
        }
        const auto externally_owned=[&](ue::UObject* cls){return std::find(protected_states.begin(),protected_states.end(),cls)==protected_states.end();};
        const auto arr = reflect::array_at(state, g_schema.states);
        if (arr.num < 0 || arr.num > 128 || (arr.num && !arr.data)) { fail("native Lily local state layout"); return; }
        std::vector<ue::UObject*> previous;
        for (int i = 0; i < arr.num; ++i) previous.push_back(reinterpret_cast<ue::UObject**>(arr.data)[i]);
        for (auto* cls : previous) if (externally_owned(cls) && std::find(wanted.begin(), wanted.end(), cls) == wanted.end()) { reflect::Frame(g_schema.remove_state).arg(0, cls).invoke(state); ++g_state_changes; }
        for (auto* cls : wanted) if (externally_owned(cls) && std::find(previous.begin(), previous.end(), cls) == previous.end()) { reflect::Frame(g_schema.add_state).arg(0, cls).invoke(state); ++g_state_changes; }
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
    // Host acceptance is an activation edge, not a per-frame survival condition.
    // A native zero-delta update can already complete that same activation.
    if(step.begin){
        if(f.persistent_class&&r.persistent_host!=f.persistent_token){
            auto& weak=g_state_classes[f.persistent_class];auto* cls=weak.get();
            if(!cls){const auto* name=g_text(f.persistent_class);if(name)cls=reflect::load_object(log::wide(*name));if(cls)weak=ObjHandle::of(cls);}
            if(!cls)return fail("persistent command class unavailable");
            if(r.persistent_restored)command_context::end_restored(pawn,r.persistent_local);
            const auto local=command_context::persistent(pawn);
            r.persistent_restored=local.cls!=cls||!local.token||local.token==r.persistent_local;
            if(r.persistent_restored){
                RestoreScope origin(pawn,{f.seat,f.generation,f.persistent_input,true});
                r.persistent_local=command_context::restore(pawn,cls,f.persistent_direction,f.persistent_button);
                if(!r.persistent_local)return;
            }else r.persistent_local=local.token;
            r.persistent_host=f.persistent_token;
        }else if(!f.persistent_class&&r.persistent_host){
            if(r.persistent_restored)command_context::end_restored(pawn,r.persistent_local);
            r.persistent_host=0;r.persistent_restored=false;
        }
    }
    if (step.begin) for (const auto& edge : f.commands) {
        if (edge.end) {
            if (r.current_host == edge.token) {
                command_context::finish(pawn, r.current_local, edge.end - 1);
                r.current_host = r.current_local = 0;
            }
            ++g_command_ends;
            continue;
        }
        const auto* name = g_text(edge.cls);
        auto& weak = g_state_classes[edge.cls];
        auto* cls = weak.get();
        if (!cls && name) { cls = reflect::load_object(log::wide(*name)); weak = ObjHandle::of(cls); }
        RestoreScope origin(pawn, {f.seat, f.generation, edge.input, true});
        r.current_local = command_context::play(pawn, cls, edge.direction, edge.button);
        if (!r.current_local) return;
        r.current_host = edge.token;
        ++g_command_starts;
    }
    g_original_tick(pawn, f.dt);
    apply_clocks(r,f);r.last=f;
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
            const float blend = !r.started || error > 250.0f ? 1.0f : 1.0f - std::exp(-20.0f * g_playout_dt);
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
        const auto now=clock::now_us();g_playout_dt=r.tick_us?static_cast<float>(now-r.tick_us)/1'000'000:1.0f/60;r.tick_us=now;
        schedule(r);
        if(r.pending.empty() && r.playing){r.world_rate=0;apply_clocks(r,r.last);causality::advance(causality::playback_ms(),0);}
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
    if (g_sink) {
        const auto seat = seat_of(pawn);
        if (seat) {
            auto& capture = g_capture[seat];
            if (capture.frame && capture.pawn.get() == pawn) {
                capture.frame->commands.swap(capture.commands);
                g_sink(pawn, *capture.frame);
                capture.frame.reset();
            }
        }
    }
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
        *g_schema.dilation.at<float>(pawn) = total / std::max(g_dt, 0.000001f);
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
    r.rate += (desired - r.rate) * (1.0f - std::exp(-4.0f * g_playout_dt));
    float budget = g_playout_dt * r.rate, total = 0, world_total = 0;
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
    *g_schema.dilation.at<float>(pawn) = total / std::max(g_dt, 0.000001f);
    r.world_rate = world_total / std::max(g_dt,0.000001f);
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
    if(auto i=g_replicas.find(seat);i!=g_replicas.end()){restore_clocks(i->second.pawn.get());command_context::forget(i->second.pawn.get());}
    g_replicas.erase(seat);
}
bool playback_gap() { return g_playback_gap; }
void reset() { g_playback_gap = false; restore_clocks();g_replicas.clear(); g_state_classes.clear(); g_text = nullptr; g_fault.clear(); command_context::reset(); causality::reset(); }
ue::UObject* actor(std::uint16_t seat) { const auto i = g_replicas.find(seat); return i == g_replicas.end() ? nullptr : i->second.pawn.get(); }
bool remote_pawn(ue::UObject* pawn) {
    return pawn && std::any_of(g_replicas.begin(), g_replicas.end(), [&](const auto& item) { return item.second.pawn.get() == pawn; });
}
void command_edge(ue::UObject* pawn, const Source& source, ue::UObject* cls, std::uint64_t token,
                  std::uint8_t direction, std::uint8_t button, std::uint8_t end) {
    if (!g_sink || !source.seat || source.seat > 8 || !source.seq || seat_of(pawn) != source.seat) return;
    auto& capture = g_capture[source.seat];
    const auto handle = ObjHandle::of(pawn);
    if (capture.pawn != handle) capture = {handle};
    if (capture.commands.size() == proto::kLilyCommands) return fail("Host command frame capacity");
    capture.commands.push_back({token, source.seq, g_name(log::utf8(cls->GetPathName())), direction, button, end});
}
bool owns(ue::UObject* object) {
    if (autonomous::owns(object)) return true;
    if (!object || g_replicas.empty() || !g_schema.ready()) return false;
    const auto* s = core::schema::script();
    for (int depth = 0; object && depth < 8; ++depth) {
        if(autonomous::owns(object))return true;
        for (const auto& [seat, r] : g_replicas) {
            auto* pawn = r.pawn.get();
            auto* pc = pawn && s ? s->pawn_controller.object(pawn) : nullptr;
            if (object == pawn || (pc && object == pc)) return true;
        }
        if(auto* pawn=autonomous::summoner(object)){object=pawn;continue;}
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
std::string fault() { return g_fault.empty() ? command_context::fault() : g_fault; }
bool source(ue::UObject* object, Source& out) {
    out = {};
    if (autonomous::source(object,out)) return true;
    if (!object || !g_schema.ready()) return false;
    const auto* s = core::schema::script();
    for (int depth = 0; object && depth < 8; ++depth) {
        if(autonomous::source(object,out))return true;
        if(g_restore_pawn.valid()&&object==g_restore_pawn.get()){out=g_restore_source;return true;}
        if(command_context::source(object,out))return true;
        if (g_sink) {
            for (std::uint16_t seat = 1; seat <= 8; ++seat) {
                auto* pawn = seat == 1 ? world::p1_pawn() : core::pawn(seat);
                auto* pc = seat == 1 ? world::p1_controller() : core::controller(seat);
                if (object == pawn || (pc && object == pc)) { out = {seat, static_cast<std::uint32_t>(ObjHandle::of(pawn).serial), g_sequence[seat],g_executing_pawn.get()==pawn}; return true; }
            }
        } else for (const auto& [seat, r] : g_replicas) {
            if (object == r.pawn.get() || object == r.controller.get()) { out = {seat, r.generation, r.playing,g_executing_pawn==r.pawn}; return true; }
        }
        if(auto* pawn=autonomous::summoner(object)){object=pawn;continue;}
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
    return std::format("native Lily: replicas={} captured={} received={} consumed={} queued={} profiles={} stateChanges={} edges={}/{} starved={} roleClocks={} clockWrites={} fault='{}'\n", g_replicas.size(), g_captured, g_received, g_consumed, queued, g_profiles, g_state_changes, g_edges_played, g_edges_received, g_starved, g_role_clocks.size(),g_clock_writes,g_fault)+std::format("  Host command edges: starts={} ends={}\n",g_command_starts,g_command_ends)+lily_events::describe()+autonomous::describe();
}
}
