#include "coop/presentation.h"

#include "engine/binary_checks.h"
#include "engine/handle.h"
#include "engine/reflect.h"
#include "base/clock.h"
#include "base/log.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace elmp::coop::presentation {
namespace {
namespace snap = net::snap;
constexpr std::size_t kIndexLimit = 256;
constexpr std::int64_t kCacheAgeUs = 60'000'000;

struct Schema {
    reflect::FuncRef component, current, name, loop, set_loop, set_animation, set_empty, clear_track, clear_tracks;
    reflect::FuncRef tick_enabled, get_time_scale, set_time_scale, set_auto_play;
    reflect::PropRef auto_playing;
    std::array<reflect::FuncRef, 8> get, set;
    ObjHandle spine_class;
    std::ptrdiff_t state = -1, begin = -1, end = -1;
    bool ready = false;

    bool live() const {
        if (!ready || !spine_class.valid()) return false;
        const reflect::FuncRef* functions[] = {&component, &current, &name, &loop, &set_loop, &set_animation, &set_empty,
            &clear_track, &clear_tracks, &tick_enabled, &get_time_scale, &set_time_scale, &set_auto_play};
        for (const auto* f : functions) if (!f->handle.valid()) return false;
        for (std::size_t i = 0; i < get.size(); ++i) if (!get[i].handle.valid() || !set[i].handle.valid()) return false;
        return true;
    }
} g_s;
struct TrackCache {
    ObjHandle entry;
    snap::PoseTrack value;
    bool present = false;
};
struct Record {
    ObjHandle spine;
    std::uintptr_t state = 0; // identity only; dereferenced only after re-reading the live component
    std::int64_t touched = 0;
    std::array<TrackCache, snap::kPoseTracks> tracks{};
    bool applied = false;
};
std::unordered_map<ObjHandle, Record> g_capture, g_apply;
std::uint32_t g_generation = 0;
std::uint64_t g_samples = 0, g_applies = 0, g_failures = 0, g_overflows = 0, g_restarts = 0, g_clears = 0, g_prunes = 0, g_reads = 0;
std::uint64_t g_paused_samples = 0, g_paused_applies = 0, g_phase_corrected = 0;
std::int64_t g_resolve_retry = 0;

bool resolve() {
    if (g_s.live()) return true;
    g_s.ready = false;
    const auto now = clock::now_us();
    if (now < g_resolve_retry) return false;
    g_resolve_retry = now + 1'000'000; // A cold reflection miss must not cause an actor-count lookup storm.
    g_s = {};
    const auto state = binary_checks::offset("SpineSkeletonAnimation.state");
    const auto begin = binary_checks::offset("SpineAnimationState.tracksBegin");
    const auto end = binary_checks::offset("SpineAnimationState.tracksEnd");
    if (!state || !begin || !end) return false;
    reflect::Resolver a("PresentationActor", reflect::find_class(L"/Script/Engine.Actor"));
    auto* cls = reflect::find_class(L"/Script/SpinePlugin.SpineSkeletonAnimationComponent");
    reflect::Resolver s("PresentationSpine", cls);
    reflect::Resolver t("PresentationTrack", reflect::find_class(L"/Script/SpinePlugin.TrackEntry"));
    g_s.component = a.func(L"GetComponentByClass", 1);
    g_s.current = s.func(L"GetCurrent", 1);
    g_s.set_animation = s.func(L"SetAnimation", 3);
    g_s.set_empty = s.func(L"SetEmptyAnimation", 2);
    g_s.clear_track = s.func(L"ClearTrack", 1);
    g_s.clear_tracks = s.func(L"ClearTracks", 0);
    g_s.tick_enabled = s.func(L"IsComponentTickEnabled", 0);
    g_s.auto_playing = s.prop(L"bAutoPlaying", reflect::Kind::Bool);
    g_s.get_time_scale = s.func(L"GetTimeScale", 0);
    g_s.set_time_scale = s.func(L"SetTimeScale", 1);
    g_s.set_auto_play = s.func(L"SetAutoPlay", 1);
    g_s.name = t.func(L"getAnimationName", 0);
    g_s.loop = t.func(L"GetLoop", 0);
    g_s.set_loop = t.func(L"SetLoop", 1);
    const wchar_t* gets[] = {L"GetTrackTime", L"GetAlpha", L"GetTimeScale", L"GetMixTime", L"GetMixDuration", L"GetAnimationStart", L"GetAnimationEnd", L"GetTrackEnd"};
    const wchar_t* sets[] = {L"SetTrackTime", L"SetAlpha", L"SetTimeScale", L"SetMixTime", L"SetMixDuration", L"SetAnimationStart", L"SetAnimationEnd", L"SetTrackEnd"};
    for (std::size_t i = 0; i < g_s.get.size(); ++i) { g_s.get[i] = t.func(gets[i], 0); g_s.set[i] = t.func(sets[i], 1); }
    if (!a.ok() || !s.ok() || !t.ok()) return false;
    g_s.spine_class = ObjHandle::of(cls);
    g_s.state = *state; g_s.begin = *begin; g_s.end = *end;
    g_s.ready = true;
    return true;
}

// Native allocations are owned by Spine, not GUObjectArray. Prefix-validated offsets plus bounded
// copy and SEH make an expired/corrupt allocation a failed sample, never an unchecked pointer walk.
bool copy_native(void* out, std::uintptr_t address, std::size_t bytes) {
    if (!address) return false;
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Active {
    std::uintptr_t state = 0;
    std::array<std::uint8_t, snap::kPoseTracks> indices{};
    std::size_t count = 0;
    bool overflow = false;
};
bool active_tracks(ue::UObject* spine, Active& out) {
    if (!copy_native(&out.state, reinterpret_cast<std::uintptr_t>(spine) + g_s.state, sizeof(out.state))) return false;
    if (!out.state) return true; // not initialized yet: an empty current set, no GetCurrent allocation
    std::uintptr_t begin = 0, end = 0;
    if (!copy_native(&begin, out.state + g_s.begin, sizeof(begin)) || !copy_native(&end, out.state + g_s.end, sizeof(end))) return false;
    if (end < begin || (end - begin) % sizeof(void*) || (end - begin) / sizeof(void*) > 4096) return false;
    const auto n = (end - begin) / sizeof(void*);
    std::array<std::uintptr_t, kIndexLimit> tracks{};
    if (n && !copy_native(tracks.data(), begin, std::min(n, kIndexLimit) * sizeof(void*))) return false;
    out.overflow = n > kIndexLimit;
    for (std::size_t i = 0; i < std::min(n, kIndexLimit); ++i) if (tracks[i]) {
        if (out.count < snap::kPoseTracks) out.indices[out.count++] = static_cast<std::uint8_t>(i);
        else out.overflow = true;
    }
    return true;
}

void prune() {
    if ((++g_reads & 255) != 0) return;
    const auto now = clock::now_us();
    const auto expired = [&](const auto& kv) { return !kv.first.valid() || now - kv.second.touched > kCacheAgeUs; };
    g_prunes += std::erase_if(g_capture, expired) + std::erase_if(g_apply, expired);
}
ue::UObject* spine_of(ue::UObject* actor, Record& rec) {
    rec.touched = clock::now_us();
    if (auto* s = rec.spine.get()) return s;
    reflect::Frame f(g_s.component);
    f.arg(0, g_s.spine_class.get());
    if (!f.invoke(actor)) return nullptr;
    auto* s = f.ret<ue::UObject*>();
    rec = {}; rec.touched = clock::now_us(); rec.spine = ObjHandle::of(s);
    return rec.spine.get();
}
ue::UObject* current(ue::UObject* spine, std::uint8_t index) {
    reflect::Frame f(g_s.current); f.arg(0, static_cast<std::int32_t>(index));
    return f.invoke(spine) ? f.ret<ue::UObject*>() : nullptr;
}
std::array<float, 8> values(const snap::PoseTrack& t) {
    return {t.t, t.alpha, t.time_scale, t.mix_time, t.mix_duration, t.animation_start, t.animation_end, t.track_end};
}
void assign_values(snap::PoseTrack& t, const std::array<float, 8>& v) {
    t.t = v[0]; t.alpha = v[1]; t.time_scale = v[2]; t.mix_time = v[3]; t.mix_duration = v[4];
    t.animation_start = v[5]; t.animation_end = v[6]; t.track_end = v[7];
}
bool sample_values(ue::UObject* entry, snap::PoseTrack& out) {
    std::array<float, 8> v{};
    for (std::size_t i = 0; i < v.size(); ++i) { reflect::Frame f(g_s.get[i]); if (!f.invoke(entry)) return false; v[i] = f.ret<float>(); }
    assign_values(out, v);
    reflect::Frame loop(g_s.loop);
    if (!loop.invoke(entry)) return false;
    out.flags = (out.flags & 2) | (loop.ret_bool() ? 1 : 0);
    return true;
}
bool sample_clock(ue::UObject* spine, float& scale, bool& paused) {
    reflect::Frame tick(g_s.tick_enabled), speed(g_s.get_time_scale);
    if (!tick.invoke(spine) || !speed.invoke(spine)) return false;
    scale = speed.ret<float>();
    if (!std::isfinite(scale) || scale < 0) return false;
    paused = !tick.ret_bool() || !g_s.auto_playing.read_bool(spine) || scale == 0;
    return true;
}
bool apply_clock(ue::UObject* spine, float scale) {
    // AutoPlay=false bypasses state.apply too. Keep the presentation component ticking and apply
    // the frozen state with a zero state clock instead; this does not silence Spine event delegates.
    if (!g_s.auto_playing.read_bool(spine)) {
        reflect::Frame f(g_s.set_auto_play); f.arg_bool(0, true);
        if (!f.invoke(spine)) return false;
    }
    reflect::Frame speed(g_s.get_time_scale);
    if (!speed.invoke(spine)) return false;
    if (speed.ret<float>() != scale) {
        reflect::Frame f(g_s.set_time_scale); f.arg(0, scale);
        if (!f.invoke(spine)) return false;
    }
    return true;
}
TrackCache* prior(Record& rec, std::uint8_t index) {
    for (auto& t : rec.tracks) if (t.present && t.value.index == index) return &t;
    return nullptr;
}
}

bool capture(ue::UObject* actor, NameIntern intern, snap::Pose& out) {
    out = {};
    if (!ue::on_game_thread() || !ue::alive(actor) || !intern || !resolve()) return ++g_failures, false;
    prune();
    auto& rec = g_capture[ObjHandle::of(actor)];
    auto* spine = spine_of(actor, rec);
    float state_scale = 1;
    bool paused = false;
    Active active;
    if (!spine || !sample_clock(spine, state_scale, paused) || !active_tracks(spine, active)) return ++g_failures, false;
    if (rec.state != active.state) { rec.tracks = {}; rec.state = active.state; }
    snap::Pose p;
    p.flags = 1 | (active.overflow ? 2 : 0) | (paused ? 4 : 0);
    p.count = static_cast<std::uint8_t>(active.count);
    std::array<TrackCache, snap::kPoseTracks> next{};
    for (std::size_t i = 0; i < active.count; ++i) {
        auto* entry = current(spine, active.indices[i]); // only non-null actual tracks, never probe empty slots
        if (!ue::alive(entry)) return ++g_failures, false;
        const auto handle = ObjHandle::of(entry);
        auto& t = p.tracks[i];
        t.index = active.indices[i];
        auto* old = prior(rec, t.index);
        if (old && old->entry == handle) {
            t.anim = old->value.anim; t.generation = old->value.generation; t.flags = old->value.flags;
        } else {
            reflect::Frame nm(g_s.name);
            if (!nm.invoke(entry)) return ++g_failures, false;
            const auto name = log::utf8(nm.ret_string());
            if (name == "<empty>") t.flags = 2;
            else if (name.empty() || !(t.anim = intern(name))) return ++g_failures, false;
            if (++g_generation == 0) ++g_generation;
            t.generation = g_generation;
        }
        if (!sample_values(entry, t)) return ++g_failures, false;
        t.time_scale *= state_scale; // Client state clock is 0/1; preserve the Host track's effective rate.
        next[i] = {handle, t, true};
    }
    if (!snap::valid_pose(p)) return ++g_failures, false;
    rec.tracks = next;
    out = p;
    ++g_samples;
    if (paused) ++g_paused_samples;
    if (active.overflow) ++g_overflows;
    return true;
}

bool apply(ue::UObject* actor, const snap::Pose& p, NameLookup lookup) {
    if (!ue::on_game_thread() || !ue::alive(actor) || !lookup || !resolve() || !(p.flags & 1) || !snap::valid_pose(p)) return ++g_failures, false;
    // Resolve all names before any mutation: a late NameDef retries the complete pose next frame.
    std::array<const std::string*, snap::kPoseTracks> names{};
    for (std::size_t i = 0; i < p.count; ++i) if (!(p.tracks[i].flags & 2) && !(names[i] = lookup(p.tracks[i].anim))) return false;
    prune();
    auto& rec = g_apply[ObjHandle::of(actor)];
    auto* spine = spine_of(actor, rec);
    Active active;
    if (!spine || !active_tracks(spine, active)) return ++g_failures, false;
    if (rec.state != active.state) { rec.tracks = {}; rec.applied = false; rec.state = active.state; }
    if (!rec.applied) {
        // State recreation/skin replacement may inherit an old pause. Restore the native clock
        // before creating the fresh tracks, then apply the incoming pause after the complete pose.
        if (!apply_clock(spine, 1)) return ++g_failures, false;
        reflect::Frame f(g_s.clear_tracks);
        if (!f.invoke(spine)) return ++g_failures, false;
        rec.applied = true; active.count = 0;
    }
    for (const auto& old : rec.tracks) if (old.present) {
        bool found = false;
        for (std::size_t i = 0; i < p.count; ++i) found |= p.tracks[i].index == old.value.index;
        if (!found) { reflect::Frame f(g_s.clear_track); f.arg(0, static_cast<std::int32_t>(old.value.index)); if (!f.invoke(spine)) return ++g_failures, false; ++g_clears; }
    }
    std::array<TrackCache, snap::kPoseTracks> next{};
    for (std::size_t i = 0; i < p.count; ++i) {
        const auto& t = p.tracks[i];
        auto* old = prior(rec, t.index);
        auto* entry = old ? old->entry.get() : nullptr;
        bool resident = false;
        for (std::size_t k = 0; k < active.count; ++k) resident |= active.indices[k] == t.index;
        // A UTrackEntry UObject can survive after its native entry was disposed. A valid ObjHandle
        // alone is insufficient: only reuse it if it remains the actual current native track.
        if (!resident || (entry && current(spine, t.index) != entry)) entry = nullptr;
        const bool restart = !entry || !old || old->value.generation != t.generation || old->value.anim != t.anim || ((old->value.flags ^ t.flags) & 2);
        if (restart) {
            if (t.flags & 2) {
                reflect::Frame f(g_s.set_empty); f.arg(0, static_cast<std::int32_t>(t.index)).arg(1, t.mix_duration);
                if (f.invoke(spine)) entry = f.ret<ue::UObject*>(); else entry = nullptr;
            } else {
                reflect::Frame f(g_s.set_animation);
                f.arg(0, static_cast<std::int32_t>(t.index)).arg_string(1, log::wide(*names[i])).arg_bool(2, (t.flags & 1) != 0);
                if (f.invoke(spine)) entry = f.ret<ue::UObject*>(); else entry = nullptr;
            }
            if (!ue::alive(entry)) return ++g_failures, false;
            ++g_restarts;
        }
        const auto v = values(t);
        const auto before = old ? values(old->value) : std::array<float, 8>{};
        for (std::size_t k = 0; k < v.size(); ++k) {
            bool set = restart || v[k] != before[k];
            if (k == 0 && !restart) {
                // Host time can remain unchanged while a Client track drifts (disabled Host tick,
                // AutoPlay, or another clock source). Always inspect actual local phase.
                reflect::Frame cur(g_s.get[0]);
                if (!cur.invoke(entry)) return ++g_failures, false;
                const float actual = cur.ret<float>();
                set = !std::isfinite(actual) || std::fabs(actual - v[0]) > 0.05f;
            }
            if (set) {
                reflect::Frame f(g_s.set[k]); f.arg(0, v[k]);
                if (!f.invoke(entry)) return ++g_failures, false;
                if (k == 0 && !restart) ++g_phase_corrected;
            }
        }
        if (!restart && ((old->value.flags ^ t.flags) & 1)) { reflect::Frame f(g_s.set_loop); f.arg_bool(0, (t.flags & 1) != 0); if (!f.invoke(entry)) return ++g_failures, false; }
        next[i] = {ObjHandle::of(entry), t, true};
    }
    if (!apply_clock(spine, (p.flags & 4) ? 0.0f : 1.0f)) return ++g_failures, false;
    rec.tracks = next;
    // SetAnimation may initialize/recreate state on first use. Observe identity again, not a stale pointer.
    Active after;
    if (active_tracks(spine, after)) rec.state = after.state;
    ++g_applies;
    if (p.flags & 4) ++g_paused_applies;
    return true;
}

void forget(ue::UObject* actor) { const auto h = ObjHandle::of(actor); g_capture.erase(h); g_apply.erase(h); }
void reset() { g_capture.clear(); g_apply.clear(); }
std::string describe() {
    return std::format("presentation: ready={} captureCache={} applyCache={} samples={} applied={} failed={} overflow={} restarted={} cleared={} pruned={} pausedSample={} pausedApply={} phaseCorrected={} tracks=8 indexMax=255\n",
        g_s.ready, g_capture.size(), g_apply.size(), g_samples, g_applies, g_failures, g_overflows, g_restarts, g_clears, g_prunes,
        g_paused_samples, g_paused_applies, g_phase_corrected);
}
} // namespace elmp::coop::presentation
