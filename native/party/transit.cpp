#include "party/transit.h"

#include "party/respawn.h"
#include "party/seat.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/frame.h"
#include "engine/handle.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/events.h"
#include "base/log.h"
#include "base/metrics.h"

#include <Unreal/FFrame.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <map>

namespace elmp::party::transit {

using namespace RC::Unreal;

namespace {

enum Fn { SwitchStart, Switched, Ready, FadeIn, FnCount };
constexpr std::array<const wchar_t*, FnCount> kPaths{
    L"/Game/_Zenith/Core/Controllers/PC_Base.PC_Base_C:OnSwitchGameMapStart",
    L"/Game/_Zenith/Core/Controllers/PC_Base.PC_Base_C:OnGameMapSwitch",
    L"/Game/_Zenith/Core/Controllers/PC_Base.PC_Base_C:OnGameMapReadyPostFade",
    L"/Game/_Zenith/UI/WBP_FaderLoading.WBP_FaderLoading_C:LaunchFadeIn",
};
constexpr std::int64_t kTimeoutUs = 20'000'000;
constexpr float kMoved = 50.0f;

std::array<std::atomic<UFunction*>, FnCount> g_fn{};
std::array<ObjHandle, FnCount> g_fn_handle{};
std::array<std::atomic<std::uint32_t>, FnCount> g_pending{}; // set by the hook, consumed by the frame phase
Hook::GlobalCallbackId g_hook{};
bool g_armed = false;

struct Run {
    bool on = false, switched = false, ready = false, placed = false, released = false;
    ObjHandle origin_pawn;
    core::Vec3 origin{};
    bool had_pawn = false;
    std::int64_t deadline = 0;
    std::map<std::int64_t, std::string> results;
    std::string placed_reason, released_reason;
};
Run g_run;
std::string g_last;
metrics::Counter& c_transits = metrics::counter("transit.done");
metrics::Counter& c_script_calls = metrics::counter("transit.hook_calls");

void on_script(Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void*) {
    UFunction* node = stack.Node();
    for (int i = 0; i < FnCount; ++i) {
        if (node != g_fn[i].load(std::memory_order_relaxed)) continue;
        ++c_script_calls;
        if (i == FadeIn || context == world::p1_controller()) ++g_pending[i];
        return;
    }
}

void resolve() {
    for (int i = 0; i < FnCount; ++i) {
        if (g_fn_handle[i].valid()) continue;
        auto* f = static_cast<UFunction*>(reflect::find_object(kPaths[i]));
        g_fn_handle[i] = ObjHandle::of(f);
        g_fn[i] = f;
        if (f) log::info("transit: hooked {}", log::utf8(kPaths[i]));
    }
}

bool p1_moved() {
    auto* p = world::p1_pawn();
    if (!p) return false;
    if (!g_run.had_pawn || g_run.origin_pawn.get() != p) return true;
    const auto l = core::location(p);
    return l && std::abs(l->y - g_run.origin.y) + std::abs(l->z - g_run.origin.z) > kMoved;
}

void place(const char* reason) {
    if (!world::p1_pawn()) return;
    g_run.placed = true;
    g_run.placed_reason = reason;
    int n = 0;
    for (const auto& seat : party::seats()) {
        const auto at = behind_p1(++n);
        if (!at) continue;
        bool revived = false;
        const auto err = respawn::place_at(seat.slot, *at, true, &revived);
        g_run.results[seat.slot] = err.empty() ? (revived ? "revived" : "moved") : "failed: " + err;
        if (!err.empty()) log::warn("transit: P{} placement failed: {}", seat.slot, err);
    }
}

void release(const char* reason) {
    if (!g_run.placed) place(reason);
    g_run.released = true;
    g_run.released_reason = reason;
    for (const auto& seat : party::seats()) respawn::release(seat.slot);
}

void finish(const char* reason) {
    if (!g_run.placed) place(reason);
    if (!g_run.released) release(reason);
    std::string res;
    for (const auto& [slot, r] : g_run.results) res += std::format(" P{}={}", slot, r);
    g_last = std::format("{} seat(s) arrived (placed: {}, released: {}, finished: {}):{}", g_run.results.size(), g_run.placed_reason, g_run.released_reason, reason, res);
    log::info("transit: {}", g_last);
    events::bus().push(events::Type::MapTransitDone, 0, static_cast<std::int64_t>(g_run.results.size()));
    ++c_transits;
    g_run = Run{};
}

void begin() {
    if (g_run.on || party::count() == 0) return;
    g_run = Run{};
    g_run.on = true;
    g_run.deadline = clock::now_us() + kTimeoutUs;
    if (auto* p = world::p1_pawn()) {
        g_run.had_pawn = true;
        g_run.origin_pawn = ObjHandle::of(p);
        if (auto l = core::location(p)) g_run.origin = *l;
    }
    for (const auto& seat : party::seats()) respawn::freeze(seat.slot);
    events::bus().push(events::Type::MapSwitchStart);
}

bool take(Fn f) { return g_pending[f].exchange(0) != 0; }

void step(const frame::Tick& t) {
    if (party::count() == 0 && !g_run.on) { // G0: no per-script-call callback without seats
        Hook::UnregisterCallback(g_hook);
        g_armed = false;
        for (auto& p : g_pending) p = 0;
        log::info("transit: script callback removed (no seats)");
        return;
    }
    if (take(SwitchStart)) begin();
    if (!g_run.on) {
        take(Switched), take(Ready), take(FadeIn); // events of transits without seats
        return;
    }
    if (take(Switched)) g_run.switched = true;
    if (take(Ready)) g_run.ready = true;
    if (take(FadeIn) && g_run.switched && !g_run.released) release("fade-in");
    if (t.now_us >= g_run.deadline) return finish("timeout");
    if (!g_run.released) {
        for (const auto& seat : party::seats()) respawn::freeze(seat.slot); // vanilla turns lost-floor pawns into falling
        if (!g_run.placed && g_run.switched && p1_moved()) place("switched");
    }
    bool loading = true, ready = false;
    if (g_run.ready || (g_run.switched && world::loader_flags(loading, ready) && !loading && ready)) finish(g_run.ready ? "ready" : "loader");
}

} // namespace

bool active() { return g_run.on; }

bool hooked() { return g_fn[SwitchStart] && g_fn[Switched] && g_fn[Ready]; }

void arm() {
    if (!g_armed) {
        Hook::FCallbackOptions opt;
        opt.OwnerModName = STR("ELMP");
        opt.HookName = STR("party.transit");
        g_hook = Hook::RegisterProcessLocalScriptFunctionPostCallback(on_script, opt);
        g_armed = true;
        log::info("transit: script callback registered");
    }
    resolve();
}

std::string describe() {
    std::string fns;
    for (int i = 0; i < FnCount; ++i) fns += std::format(" {}={}", i, g_fn[i] ? "ok" : "-");
    return std::format("active={} hooked={} fns:{} calls={} done={}\nlast: {}\n", g_run.on, hooked(), fns, c_script_calls.load(), c_transits.load(), g_last.empty() ? "-" : g_last);
}

void install() {
    frame::add_phase("party.transit", step, [] { return g_armed; });
    frame::add_phase("party.transit.resolve", [](const frame::Tick&) { resolve(); }, [] { return g_armed && !(hooked() && g_fn[FadeIn]); }, 1000);
    lifecycle::on_teardown([] {
        g_run = Run{};
        for (auto& p : g_pending) p = 0;
    });
}

} // namespace elmp::party::transit
