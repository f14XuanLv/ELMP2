#include "coop/native_lily.h"
#include "coop/reaction_color.h"

#include "base/clock.h"
#include "base/hook.h"
#include "base/preflight.h"
#include "engine/reflect.h"
#include "engine/world.h"
#include "guard/guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>

namespace elmp::coop::reaction::color {
namespace {
using PlayFn = void(__fastcall*)(ue::UObject*);
using Delegate = RC::Unreal::FScriptDelegate;
PlayFn o_play{};
void (*g_sink)(const Capture&) = nullptr;
bool (*g_filter)(ue::UObject*) = nullptr;
bool g_installed = false, g_client = false, g_replaying = false;
std::uint64_t g_captured = 0, g_capture_failed = 0, g_replayed = 0, g_rejected = 0;
std::string g_failure, g_host_fault;

constexpr auto kPlayer = L"/Game/_Zenith/Core/Pawns/BP_Character_Player_Base.BP_Character_Player_Base_C";
constexpr auto kCurve = L"/Game/_Zenith/Gameplay/Curves/Curve_PlayerDamageColor.Curve_PlayerDamageColor";
constexpr auto kColorProperty = L"DamageColorCurve_Color_899AC2694465CC6281559880FBA7924D";
constexpr auto kDirectionProperty = L"DamageColorCurve__Direction_899AC2694465CC6281559880FBA7924D";

struct Schema {
    std::vector<ObjHandle> types;
    ObjHandle component_class, player_class, color_class, curve;
    reflect::PropRef timeline, player_timeline, player_color, player_color_value, player_direction;
    reflect::PropRef events, vectors, floats, colors, length_mode, looping, play_rate;
    reflect::PropRef property_object, direction_name, update_delegate, finished_delegate;
    reflect::PropRef track_curve, track_name, color_name, interp_delegate;
    reflect::FuncRef play, playing, reversing, position, update, finished;
    ue::FName color_property, direction_property, color_track, update_name, finished_name;
    bool live() const {
        return curve.valid() && std::all_of(types.begin(), types.end(), [](const auto& h) { return h.valid(); }) &&
            play.handle.valid() && playing.handle.valid() && reversing.handle.valid() && position.handle.valid() &&
            update.handle.valid() && finished.handle.valid();
    }
};
// No actor/component pointer is cached across calls. Type/function/curve serials plus reset invalidate
// the schema; failed cold resolution is retried. Runtime delegate/track context is validated each start.
std::optional<Schema> g_schema;

bool within(const reflect::PropRef& p, std::int32_t size) {
    return p.ok() && p.size > 0 && p.offset <= size && p.size <= size - p.offset;
}
bool void_call(const reflect::FuncRef& f) { return f.ok() && f.count == 0 && f.ret < 0 && f.parms_size == 0; }
bool getter(const reflect::FuncRef& f, reflect::Kind kind, std::int32_t size) {
    return f.ok() && f.count == 1 && f.ret == 0 && f.parms[0].kind == kind && f.parms[0].size == size;
}

const Schema* schema() {
    if (g_schema && g_schema->live()) return &*g_schema;
    g_schema.reset();
    Schema s;
    auto type = [&](const wchar_t* path) {
        auto* p = static_cast<ue::UStruct*>(reflect::find_object(path));
        s.types.push_back(ObjHandle::of(p)); return p;
    };
    auto* component = type(L"/Script/Engine.TimelineComponent");
    auto* timeline = type(L"/Script/Engine.Timeline");
    auto* track = type(L"/Script/Engine.TimelineLinearColorTrack");
    auto* player = type(kPlayer);
    s.component_class = ObjHandle::of(component); s.player_class = ObjHandle::of(player);
    s.color_class = ObjHandle::of(type(L"/Script/Zenith.ColorComponent"));
    auto* curve_class = type(L"/Script/Engine.CurveLinearColor");
    s.curve = ObjHandle::of(reflect::find_object(kCurve));
    reflect::Resolver c("DamageColor TimelineComponent", component), t("DamageColor Timeline", timeline);
    reflect::Resolver r("DamageColor LinearColorTrack", track), p("DamageColor PlayerBase", player);
    s.timeline = c.prop(L"TheTimeline", reflect::Kind::Struct);
    s.play = c.func(L"PlayFromStart", 0);
    s.playing = c.func(L"IsPlaying", 0);
    s.reversing = c.func(L"IsReversing", 0);
    s.position = c.func(L"GetPlaybackPosition", 0);
    s.player_timeline = p.prop(L"DamageColorCurve", reflect::Kind::Object, sizeof(void*));
    s.player_color = p.prop(L"ColorComponent", reflect::Kind::Object, sizeof(void*));
    s.player_color_value = p.prop(kColorProperty, reflect::Kind::Struct, 16);
    s.player_direction = p.prop(kDirectionProperty, reflect::Kind::Byte, 1);
    s.update = p.func(L"DamageColorCurve__UpdateFunc", 0);
    s.finished = p.func(L"DamageColorCurve__FinishedFunc", 0);
    s.events = t.prop(L"Events", reflect::Kind::Array, sizeof(reflect::RawArray));
    s.vectors = t.prop(L"InterpVectors", reflect::Kind::Array, sizeof(reflect::RawArray));
    s.floats = t.prop(L"InterpFloats", reflect::Kind::Array, sizeof(reflect::RawArray));
    s.colors = t.prop(L"InterpLinearColors", reflect::Kind::Array, sizeof(reflect::RawArray));
    s.length_mode = t.prop(L"LengthMode", reflect::Kind::Byte, 1);
    s.looping = t.prop(L"bLooping", reflect::Kind::Bool, 1);
    s.play_rate = t.prop(L"PlayRate", reflect::Kind::Float, 4);
    s.property_object = t.prop(L"PropertySetObject", reflect::Kind::Weak, sizeof(RC::Unreal::FWeakObjectPtr));
    s.direction_name = t.prop(L"DirectionPropertyName", reflect::Kind::Name, sizeof(ue::FName));
    s.update_delegate = t.prop(L"TimelinePostUpdateFunc", reflect::Kind::Delegate, sizeof(Delegate));
    s.finished_delegate = t.prop(L"TimelineFinishedFunc", reflect::Kind::Delegate, sizeof(Delegate));
    s.track_curve = r.prop(L"LinearColorCurve", reflect::Kind::Object, sizeof(void*));
    s.track_name = r.prop(L"TrackName", reflect::Kind::Name, sizeof(ue::FName));
    s.color_name = r.prop(L"LinearColorPropertyName", reflect::Kind::Name, sizeof(ue::FName));
    s.interp_delegate = r.prop(L"InterpFunc", reflect::Kind::Delegate, sizeof(Delegate));
    if (!c.ok() || !t.ok() || !r.ok() || !p.ok()) {
        g_failure = c.failure() + t.failure() + r.failure() + p.failure(); return nullptr;
    }
    auto* inner = static_cast<RC::Unreal::FArrayProperty*>(s.colors.prop)->GetInner();
    auto* timeline_type = static_cast<RC::Unreal::FStructProperty*>(s.timeline.prop)->GetStruct().Get();
    const auto track_size = reflect::properties_size(track);
    bool layout = timeline_type == timeline && inner && reflect::kind_of(inner) == reflect::Kind::Struct &&
        static_cast<RC::Unreal::FStructProperty*>(inner)->GetStruct().Get() == track &&
        inner->GetSize() == track_size && track_size > 0 && track_size <= 1024 &&
        s.timeline.size == reflect::properties_size(timeline) && s.timeline.size <= 2048 &&
        within(s.timeline, reflect::properties_size(component));
    for (const auto& field : {s.events, s.vectors, s.floats, s.colors, s.length_mode, s.looping, s.play_rate,
                              s.property_object, s.direction_name, s.update_delegate, s.finished_delegate})
        layout = layout && within(field, s.timeline.size);
    for (const auto& field : {s.track_curve, s.track_name, s.color_name, s.interp_delegate})
        layout = layout && within(field, track_size);
    for (const auto& field : {s.player_timeline, s.player_color, s.player_color_value, s.player_direction})
        layout = layout && within(field, reflect::properties_size(player));
    if (!layout || !void_call(s.play) || !void_call(s.update) || !void_call(s.finished) ||
        !getter(s.playing, reflect::Kind::Bool, 1) || !getter(s.reversing, reflect::Kind::Bool, 1) ||
        !getter(s.position, reflect::Kind::Float, 4) || !s.live() ||
        !s.curve.get()->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(curve_class))) {
        g_failure = "DamageColor reflected Timeline/track/zero-argument ABI/curve context mismatch"; return nullptr;
    }
    s.color_property = ue::FName(kColorProperty, RC::Unreal::FNAME_Add);
    s.direction_property = ue::FName(kDirectionProperty, RC::Unreal::FNAME_Add);
    s.color_track = ue::FName(L"Color", RC::Unreal::FNAME_Add);
    s.update_name = ue::FName(L"DamageColorCurve__UpdateFunc", RC::Unreal::FNAME_Add);
    s.finished_name = ue::FName(L"DamageColorCurve__FinishedFunc", RC::Unreal::FNAME_Add);
    g_schema = std::move(s); return &*g_schema;
}

bool array_shape(const reflect::RawArray& a, std::int32_t count) {
    return a.num == count && a.max >= a.num && a.max <= 1024 && (!a.num || a.data);
}
bool context(const Schema& s, ue::UObject* actor, ue::UObject* comp) {
    if (!ue::alive(actor) || !ue::alive(comp) || comp->GetOuterPrivate() != actor ||
        !actor->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(s.player_class.get())) ||
        comp->GetClassPrivate() != s.component_class.get() || s.player_timeline.object(actor) != comp) {
        g_failure = "DamageColor is not this PlayerBase's exact TimelineComponent"; return false;
    }
    auto* color = s.player_color.object(actor);
    if (!color || color->GetOuterPrivate() != actor ||
        !color->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(s.color_class.get()))) {
        g_failure = "DamageColor target ColorComponent is not initialized for this actor"; return false;
    }
    const auto* time = s.timeline.at<std::uint8_t>(comp);
    const auto& colors = reflect::array_at(time, s.colors);
    if (!array_shape(reflect::array_at(time, s.events), 0) || !array_shape(reflect::array_at(time, s.vectors), 0) ||
        !array_shape(reflect::array_at(time, s.floats), 0) || !array_shape(colors, 1)) {
        g_failure = "DamageColor requires exactly one color track and no other tracks"; return false;
    }
    const auto* update = s.update_delegate.at<Delegate>(time);
    const auto* finished = s.finished_delegate.at<Delegate>(time);
    const auto* interp = s.interp_delegate.at<Delegate>(colors.data);
    // CreateTimelineComponent installs no per-track delegate: it writes the reflected color property,
    // then calls the exact pure Blueprint Update on the same actor. Do not admit derived overrides.
    if (s.property_object.at<RC::Unreal::FWeakObjectPtr>(time)->Get() != actor ||
        s.direction_name.read<ue::FName>(time) != s.direction_property ||
        update->GetUObject() != actor || update->GetFunctionName() != s.update_name ||
        finished->GetUObject() != actor || finished->GetFunctionName() != s.finished_name ||
        actor->GetFunctionByNameInChain(s.update_name) != s.update.fn ||
        actor->GetFunctionByNameInChain(s.finished_name) != s.finished.fn ||
        interp->GetUObject() || interp->GetFunctionName() != ue::FName() ||
        s.track_curve.object(colors.data) != s.curve.get() ||
        s.track_name.read<ue::FName>(colors.data) != s.color_track ||
        s.color_name.read<ue::FName>(colors.data) != s.color_property ||
        s.length_mode.read<std::uint8_t>(time) != 1 || s.looping.read_bool(time) ||
        s.play_rate.read<float>(time) != 1.0f) {
        g_failure = "DamageColor original curve/property/delegate/playback context changed"; return false;
    }
    return true;
}

bool started(const Schema& s, ue::UObject* comp) {
    reflect::Frame playing(s.playing), reversing(s.reversing), position(s.position);
    if (!playing.invoke(comp) || !reversing.invoke(comp) || !position.invoke(comp) ||
        !playing.ret_bool() || reversing.ret_bool() || !std::isfinite(position.ret<float>()) ||
        std::fabs(position.ret<float>()) > 0.00001f) {
        g_failure = "DamageColor native PlayFromStart did not start forward at zero"; return false;
    }
    return true;
}

void capture_failed() {
    ++g_capture_failed;
    if (g_host_fault.empty()) g_host_fault = g_failure.empty() ? "DamageColor capture failed" : g_failure;
}
void __fastcall d_play(ue::UObject* comp) {
    auto* actor = ue::alive(comp) ? comp->GetOuterPrivate() : nullptr;
    if (!g_sink || g_replaying || !ue::on_game_thread() || !ue::alive(actor) || (g_filter && !g_filter(actor))) {
        o_play(comp); return;
    }
    const auto* s = schema();
    if (!s) { o_play(comp); capture_failed(); return; }
    if (!actor->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(s->player_class.get())) ||
        s->player_timeline.object(actor) != comp) {
        o_play(comp); return;
    }
    Capture cap;
    cap.entry_order = next_capture_order();
    cap.event.kind = 2;
    cap.event.t_ms = static_cast<std::uint32_t>(clock::now_us() / 1000);
    const auto actor_handle = ObjHandle::of(actor), comp_handle = ObjHandle::of(comp);
    const bool ready = cap.entry_order && context(*s, actor, comp);
    o_play(comp);
    if (!ready || actor_handle.get() != actor || comp_handle.get() != comp || !started(*s, comp)) {
        if (ready && (actor_handle.get() != actor || comp_handle.get() != comp))
            g_failure = "DamageColor owner/component expired during native start";
        capture_failed(); return;
    }
    cap.owner = actor;
    ++g_captured;
    if (g_sink) g_sink(cap);
}

bool install() {
    if (g_installed) return true;
    const auto* s = schema();
    if (!s) return false;
    auto* exec = reinterpret_cast<const std::uint8_t*>(s->play.fn->GetFuncPtr());
    if (!exec || exec != preflight::address("ColorDamagePlayFromStartExec") || exec[0x15] != 0xE9) {
        g_failure = "DamageColor reflected PlayFromStart exec/jump mismatch"; return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, exec + 0x16, sizeof(displacement));
    auto* target = const_cast<std::uint8_t*>(exec + 0x1A + displacement);
    if (target != preflight::address("ColorDamagePlayFromStart")) {
        g_failure = "DamageColor PlayFromStart native target mismatch"; return false;
    }
    g_installed = hook::install_address("ColorDamagePlayFromStart", target, reinterpret_cast<void*>(&d_play),
        reinterpret_cast<void**>(&o_play), "reflect");
    if (!g_installed) g_failure = "DamageColor native detour refused";
    return g_installed;
}
} // namespace

bool host_enable(void (*sink)(const Capture&), bool (*filter)(ue::UObject*)) {
    if (!g_sink && sink) g_host_fault.clear();
    if (!install()) return false;
    g_sink = sink; g_filter = filter; return true;
}
void host_disable() { g_sink = nullptr; g_filter = nullptr; g_host_fault.clear(); }
bool client_enable(bool on) {
    if (on && !install()) return false;
    g_client = on; return true;
}
bool replay(ue::UObject* actor, const net::proto::Reaction& event, std::string* why) {
    const auto fail = [&](std::string reason) {
        g_failure = std::move(reason); ++g_rejected; if (why) *why = g_failure; return false;
    };
    if (!g_client || !guard::armed() || !ue::on_game_thread() || !ue::alive(actor) || (actor != world::p1_pawn() && !native_lily::owns(actor)))
        return fail("DamageColor requires armed Client controlled Lily on game thread");
    if (event.kind != 2 || !net::proto::valid_reaction(event)) return fail("malformed DamageColor reaction");
    const auto* s = schema();
    if (!s) return fail(g_failure);
    if (!actor->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(s->player_class.get())))
        return fail("DamageColor actor is not verified PlayerBase");
    auto* comp = s->player_timeline.object(actor);
    if (!context(*s, actor, comp)) return fail(g_failure);
    struct ReplayScope {
        bool prior = g_replaying;
        ReplayScope() { g_replaying = true; }
        ~ReplayScope() { g_replaying = prior; }
    } scope;
    const auto handle = ObjHandle::of(comp);
    reflect::Frame call(s->play);
    if (!call.invoke(comp)) return fail("DamageColor PlayFromStart unavailable");
    if (handle.get() != comp || !started(*s, comp)) return fail(g_failure);
    ++g_replayed; return true;
}
std::string host_fault() { return g_host_fault; }
void reset() { g_schema.reset(); g_failure.clear(); g_host_fault.clear(); }
std::string describe() {
    return std::format("  damageColor installed={} host={} client={} captured={} captureFailed={} replayed={} rejected={} schema={} failure={} fault={}\n",
        g_installed, g_sink != nullptr, g_client, g_captured, g_capture_failed, g_replayed, g_rejected,
        g_schema && g_schema->live(), g_failure, g_host_fault);
}

} // namespace elmp::coop::reaction::color
