#include "coop/causality.h"
#include "coop/command_context.h"
#include "coop/ability.h"
#include "coop/feedback_context.h"
#include "coop/native_lily.h"
#include "coop/causal_facts.h"
#include "coop/autonomous.h"
#include "engine/reflect.h"
#include "base/clock.h"
#include "base/hook.h"
#include "base/json.h"
#include "base/log.h"
#include "base/metrics.h"
#include <algorithm>
#include <deque>
#include <format>
#include <unordered_map>

namespace elmp::coop::causality {
namespace {
struct Bound { ObjHandle actor, invoker; ability::Identity id; native_lily::Source source; unsigned ticks = 0; float age = 0; };
std::unordered_map<std::uint64_t, Bound> g_bound;
std::unordered_map<std::uint64_t, native_lily::Source> g_roots;
std::deque<json::Value> g_trace;
bool g_enabled = false, g_tracing = false;
unsigned g_trace_stride = 5;
std::uint64_t g_queries = 0, g_nonnull = 0, g_ticks = 0, g_launches = 0, g_stops = 0, g_ended = 0;
using Query = ue::UObject*(__fastcall*)(ue::UObject*, float, float, bool, std::uint8_t, bool);
using Tick = void(__fastcall*)(ue::UObject*, float, std::int32_t, void*);
Query g_query = nullptr;
Tick g_tick = nullptr;
using SetTarget = void(__fastcall*)(ue::UObject*, ue::UObject*);
SetTarget g_set_target = nullptr;
reflect::FuncRef g_target, g_location, g_velocity, g_actor_location;
reflect::PropRef g_command_pawn;
struct SpiritOrigin { ObjHandle command, pawn; native_lily::Source source; };
std::unordered_map<ObjHandle, SpiritOrigin> g_spirits;
std::uint64_t g_summons = 0, g_presented_fx = 0, g_presented_sound = 0, g_captured_fx = 0, g_captured_sound = 0;
auto& h_feedback = metrics::histogram("causal.feedback_late_ms");
using SpiritStart = std::uintptr_t(__fastcall*)(ue::UObject*, ue::UObject*);
using SpiritCommands = std::uintptr_t(__fastcall*)(ue::UObject*, ue::UObject*, void*);
SpiritStart g_spirit_start = nullptr;
SpiritCommands g_spirit_commands = nullptr;
void prune_origins() {
    std::erase_if(g_spirits, [](const auto& p) {
        native_lily::Source current;
        return !p.first.valid() || !p.second.command.valid() || !p.second.pawn.valid() ||
            !native_lily::source(p.second.pawn.get(), current) || current.seat != p.second.source.seat || current.generation != p.second.source.generation;
    });
}
void spirit_origin(ue::UObject* spirit, ue::UObject* command) {
    if (!g_enabled || !ue::on_game_thread()) return;
    prune_origins();
    const auto handle = ObjHandle::of(spirit);
    g_spirits.erase(handle);
    auto* pawn = command ? g_command_pawn.object(command) : nullptr;
    native_lily::Source source;
    if (!pawn || !native_lily::source(pawn, source) || !source.seq || g_spirits.size() >= 128) return;
    // This is the original summon-command boundary, before OnAppear/command execution.
    source.command_tick = true;
    g_spirits.emplace(handle, SpiritOrigin{ObjHandle::of(command), ObjHandle::of(pawn), source});
    ++g_summons;
}
std::uintptr_t __fastcall spirit_start(ue::UObject* spirit, ue::UObject* command) {
    if(autonomous::defer_start(spirit))return 0;
    autonomous::summoned(spirit, command);
    spirit_origin(spirit, command);
    return g_spirit_start(spirit, command);
}
std::uintptr_t __fastcall spirit_commands(ue::UObject* spirit, ue::UObject* command, void* commands) {
    if(autonomous::defer_start(spirit))return 0;
    autonomous::summoned(spirit, command);
    spirit_origin(spirit, command);
    return g_spirit_commands(spirit, command, commands);
}
bool origin(ue::UObject* invoker, native_lily::Source& source) {
    if (autonomous::source(invoker, source)) return true;
    prune_origins();
    if (const auto i = g_spirits.find(ObjHandle::of(invoker)); i != g_spirits.end()) { source = i->second.source; return true; }
    return native_lily::source(invoker, source);
}
std::string label(ue::UObject* o) { return ue::alive(o) ? log::utf8(o->GetFullName()) : "null"; }
std::string path(const net::proto::AbilityPath& p) {
    std::string out;
    for (unsigned i = 0; i < p.depth; ++i) out += std::format("{}{}:{}", i ? "/" : "", p.entries[i], p.occurrences[i]);
    return out;
}
json::Value event(std::string_view type, const Bound& b) {
    auto j = json::Value::object();
    j.set("type", std::string(type)); j.set("t_us", clock::now_us());
    j.set("seat", b.source.seat); j.set("generation", static_cast<std::int64_t>(b.source.generation));
    j.set("input", static_cast<std::int64_t>(b.source.seq));
    j.set("domain", b.source.domain);
    j.set("activation", static_cast<std::int64_t>(b.id.token)); j.set("root", static_cast<std::int64_t>(b.id.root));
    j.set("path", path(b.id.path)); j.set("class", b.id.cls); j.set("invoker", label(b.invoker.get()));
    j.set("object",label(b.actor.get()));
    j.set("wire_root",static_cast<std::int64_t>(wire_root(b.id.root)));
    return j;
}
void append(json::Value j) {
    if (!g_tracing) return;
    if (g_trace.size() == 1024) g_trace.pop_front();
    g_trace.push_back(std::move(j));
}
void vector(json::Value& j, const char* key, reflect::FuncRef f, ue::UObject* object) {
    reflect::Frame frame(f);
    if (!object || !frame.invoke(object)) return;
    const auto v = frame.ret<std::array<float, 3>>();
    auto a = json::Value::array(); for (float n : v) a.push(n);
    j.set(key, std::move(a));
}
ue::UObject* __fastcall query(ue::UObject* actor, float min, float range, bool los, std::uint8_t channel, bool front) {
    auto* result = g_query(actor, min, range, los, channel, front);
    ability::Identity id;
    if (!g_enabled || !ue::on_game_thread() || !ability::current(id)) return result;
    const auto it = g_bound.find(id.token);
    if (it == g_bound.end()) return result;
    auto* original = result;
    result = facts::query(id, actor, min, range, los, channel, front, result);
    ++g_queries; if (result) ++g_nonnull;
    if (g_tracing) {
        auto j = event("query", it->second);
        j.set("actor", label(actor)); j.set("target", label(result)); j.set("min", min); j.set("range", range);
        j.set("local_target", label(original)); j.set("wire_root", static_cast<std::int64_t>(current_root()));
        j.set("los", los); j.set("front", front); j.set("channel", channel);
        append(std::move(j));
    }
    return result;
}
void __fastcall tick(ue::UObject* component, float dt, std::int32_t type, void* function) {
    ability::NativeScope scope(component ? component->GetOuterPrivate() : nullptr);
    if (g_enabled && ue::on_game_thread() && component) {
        ability::Identity id;
        if (ability::identity(component->GetOuterPrivate(), id)) if (auto it = g_bound.find(id.token); it != g_bound.end()) {
            if(!facts::homing(component, id, dt))return;
            ++g_ticks;
            it = g_bound.find(id.token);
            if(it==g_bound.end()){g_tick(component,dt,type,function);return;}
            if (g_tracing && it->second.ticks++ % g_trace_stride == 0) {
                reflect::Frame target(g_target);
                auto* t = target.invoke(component) ? target.ret<ue::UObject*>() : nullptr;
                auto j = event("homing", it->second); j.set("dt", dt);j.set("age",it->second.age); j.set("target_component", label(t));
                vector(j, "target_position", g_location, t); vector(j, "target_velocity", g_velocity, t);
                vector(j, "projectile_position", g_actor_location, it->second.actor.get());
                append(std::move(j));
            }
            it->second.age+=dt;
        }
    }
    g_tick(component, dt, type, function);
}
void __fastcall set_target(ue::UObject* component, ue::UObject* target) {
    if (g_enabled && ue::on_game_thread()) target = facts::target(component, target);
    g_set_target(component, target);
}
}
bool enable() {
    if (g_enabled) return true;
    reflect::Resolver h("causal Homing", reflect::find_class(L"/Script/Zenith.HomingComponent"));
    g_target = h.func(L"GetTarget", 0);
    reflect::Resolver s("causal target", reflect::find_class(L"/Script/Engine.SceneComponent"));
    g_location = s.func(L"K2_GetComponentLocation", 0); g_velocity = s.func(L"GetComponentVelocity", 0);
    g_actor_location = reflect::Resolver("causal actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"K2_GetActorLocation", 0);
    reflect::Resolver command("causal summon command", reflect::find_class(L"/Script/Zenith.CommandAction"));
    g_command_pawn = command.prop(L"Pawn", reflect::Kind::Object);
    if (!h.ok() || !s.ok() || !command.ok()) return false;
    if (!g_spirit_start && !hook::install_manifest("SpiritStartNative", reinterpret_cast<void*>(&spirit_start), reinterpret_cast<void**>(&g_spirit_start))) return false;
    if (!g_spirit_commands && !hook::install_manifest("SpiritCommandsNative", reinterpret_cast<void*>(&spirit_commands), reinterpret_cast<void**>(&g_spirit_commands))) return false;
    if (!g_query && !hook::install_manifest("HomingQueryNative", reinterpret_cast<void*>(&query), reinterpret_cast<void**>(&g_query))) return false;
    if (!g_tick && !hook::install_manifest("HomingTickNative", reinterpret_cast<void*>(&tick), reinterpret_cast<void**>(&g_tick))) return false;
    if (!g_set_target && !hook::install_manifest("HomingSetTargetNative", reinterpret_cast<void*>(&set_target), reinterpret_cast<void**>(&g_set_target))) return false;
    if (!facts::install()) return false;
    return g_enabled = true;
}
void reset() { autonomous::reset(); g_enabled = false; g_bound.clear(); g_roots.clear(); g_spirits.clear(); facts::clear(); }
void prepare_reuse(ue::UObject* actor){facts::prepare_reuse(actor);}
void launched(ue::UObject* actor, ue::UObject* invoker, const ability::Identity& id) {
    if (!g_enabled) return;
    native_lily::Source source;
    if (id.token == id.root) {
        if (!origin(invoker, source)) return;
        g_roots[id.root] = source;
    } else {
        const auto it = g_roots.find(id.root); if (it == g_roots.end()) return; source = it->second;
    }
    std::erase_if(g_bound, [](const auto& p) { return !p.second.actor.valid() || !p.second.id.active; });
    std::erase_if(g_roots, [](const auto& p) { return !ability::has_active_family(p.first); });
    if (g_bound.size() >= 4096 || g_roots.size() >= 1024) return;
    Bound b{ObjHandle::of(actor), ObjHandle::of(invoker), id, source};
    facts::launch(actor, invoker, id, source);
    g_bound[id.token] = b; ++g_launches;
    if (g_tracing) { auto j=event("launch",b);vector(j,"projectile_position",g_actor_location,actor);vector(j,"invoker_position",g_actor_location,invoker);append(std::move(j)); }
}
void stop_requested(ue::UObject*, const ability::Identity& id, std::uint8_t reason) {
    facts::stopping(id, reason);
    if (const auto it = g_bound.find(id.token); g_enabled && it != g_bound.end()) {
        ++g_stops; if (g_tracing) { auto j = event("stop", it->second); j.set("reason", reason); append(std::move(j)); }
    }
}
void ended(ue::UObject*, const ability::Identity& id) {
    facts::end(id);
    if (const auto it = g_bound.find(id.token); g_enabled && it != g_bound.end()) {
        ++g_ended; if (g_tracing) append(event("ended", it->second)); g_bound.erase(it);
    }
}
bool retained_source(const native_lily::Source& source) {
    if(command_context::retained_source(source))return true;
    prune_origins();
    return std::any_of(g_spirits.begin(), g_spirits.end(), [&](const auto& p) { const auto& s=p.second.source; return s.seat==source.seat && s.generation==source.generation && s.seq==source.seq && s.domain==source.domain; });
}
void captured(std::uint64_t wire, std::uint32_t time, bool sound) {
    if(!wire)return;
    ++(sound?g_captured_sound:g_captured_fx);
    if(g_tracing){auto j=json::Value::object();j.set("type",sound?"feedback_sound_capture":"feedback_fx_capture");j.set("wire_root",static_cast<std::int64_t>(wire));j.set("host_ms",static_cast<std::int64_t>(time));j.set("source",label(feedback_context::effect_source()));j.set("t_us",clock::now_us());append(std::move(j));}
}
void presented(std::uint64_t wire, std::uint32_t time, bool sound) {
    if (!wire) return;
    ++(sound ? g_presented_sound : g_presented_fx);
    h_feedback.add(static_cast<float>(std::max(0.0, playback_ms()-time)));
    if (g_tracing) {
        auto j=json::Value::object();j.set("type",sound?"feedback_sound":"feedback_fx");j.set("wire_root",static_cast<std::int64_t>(wire));j.set("host_ms",static_cast<std::int64_t>(time));j.set("playback_ms",playback_ms());j.set("t_us",clock::now_us());append(std::move(j));
    }
}
std::string describe() { return std::format("causality: enabled={} live={} roots={} launched={} queries={} nonnull={} homingTicks={} stops={} ended={} trace={} summons={} origins={} feedbackFx={} feedbackSound={} capturedFx={} capturedSound={}\n", g_enabled, g_bound.size(), g_roots.size(), g_launches, g_queries, g_nonnull, g_ticks, g_stops, g_ended, g_trace.size(), g_summons, g_spirits.size(), g_presented_fx, g_presented_sound, g_captured_fx, g_captured_sound) + facts::describe(); }
std::string trace(std::string_view action) {
    if (action == "on" || action == "dense") { g_trace.clear(); g_tracing = true;g_trace_stride=action=="dense"?1:5; return "causal trace enabled\n"; }
    if (action == "off") { g_tracing = false; return "causal trace disabled\n"; }
    auto out = json::Value::array(); for (const auto& j : g_trace) out.push(j);
    return json::dump(out, 0) + "\n";
}
}
