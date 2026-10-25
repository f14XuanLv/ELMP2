#include "party/ui.h"

#include "party/seat.h"

#include "core/schema.h"

#include "engine/frame.h"
#include "engine/handle.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/log.h"
#include "base/metrics.h"

#include "overlay/render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace elmp::party::ui {

using reflect::Kind;

namespace {

struct Schema {
    reflect::FuncRef set_paused, project, special_cost, in_viewport, display_ui;
    reflect::PropRef widget_slot, widget_visibility;
    ObjHandle gs, logic, settings_class;
};
std::optional<Schema> g_s;
std::string g_failure;

unsigned g_holders = 0;
bool g_we_paused = false;

bool g_menu_open = false, g_display = true;
std::int64_t g_menu_at = 0, g_display_at = 0;
std::uint64_t g_menu_version = ~0ull;
ObjHandle g_settings;
float g_cost = -1;
metrics::Counter& c_menu_checks = metrics::counter("ui.menu_checks");

const Schema* schema() {
    if (g_s || !g_failure.empty()) return g_s ? &*g_s : nullptr;
    Schema s;
    reflect::Resolver gs("GameplayStatics", reflect::find_class(L"/Script/Engine.GameplayStatics"));
    s.set_paused = gs.func(L"SetGamePaused", 2);
    s.project = gs.func(L"ProjectWorldToScreen", 4);
    reflect::Resolver lg("GameLogicBPFLibrary", reflect::find_class(L"/Script/Zenith.GameLogicBPFLibrary"));
    s.special_cost = lg.func(L"GetSpecialAttackMPCost", 0);
    reflect::Resolver uw("UserWidget", reflect::find_class(L"/Script/UMG.UserWidget"));
    s.in_viewport = uw.func(L"IsInViewport", 0);
    reflect::Resolver w("Widget", reflect::find_class(L"/Script/UMG.Widget"));
    s.widget_slot = w.prop(L"Slot", Kind::Object);
    s.widget_visibility = w.prop(L"Visibility", Kind::Any, 1);
    auto* settings = reflect::find_class(L"/Script/Zenith.GameSettingsSubsystem");
    reflect::Resolver gss("GameSettingsSubsystem", settings);
    s.display_ui = gss.func(L"GetDisplayPlayerUI", 0);
    for (const auto* r : {&gs, &lg, &uw, &w, &gss})
        if (!r->ok() && g_failure.empty()) g_failure = r->failure();
    s.gs = ObjHandle::of(reflect::find_object(L"/Script/Engine.Default__GameplayStatics"));
    s.logic = ObjHandle::of(reflect::find_object(L"/Script/Zenith.Default__GameLogicBPFLibrary"));
    s.settings_class = ObjHandle::of(settings);
    if (g_failure.empty() && (s.gs.empty() || s.logic.empty())) g_failure = "library CDOs missing";
    if (!g_failure.empty()) {
        log::error("party ui services unavailable: {}", g_failure);
        return nullptr;
    }
    g_s = s;
    return &*g_s;
}

bool set_paused(bool on) {
    const auto* s = schema();
    auto* w = world::world();
    if (!s || !w) return false;
    reflect::Frame f(s->set_paused);
    f.arg(0, w).arg_bool(1, on);
    return f.invoke(s->gs.get());
}

} // namespace

void hold_pause(Holder who, bool on) {
    const unsigned before = g_holders;
    g_holders = on ? (g_holders | who) : (g_holders & ~static_cast<unsigned>(who));
    if (!before && g_holders && !world::paused()) g_we_paused = set_paused(true);
    else if (before && !g_holders && g_we_paused) {
        set_paused(false);
        g_we_paused = false;
    }
}

unsigned pause_holders() { return g_holders; }

bool vanilla_menu_open() {
    const auto now = clock::now_us();
    if (registry::version(registry::Kind::Menu) == g_menu_version && now - g_menu_at < 250'000) return g_menu_open;
    g_menu_version = registry::version(registry::Kind::Menu);
    g_menu_at = now;
    g_menu_open = false;
    const auto* s = schema();
    if (!s) return false;
    // Only top-level (no panel slot), visible widgets can be menus on screen: a direct-read filter keeps the
    // ProcessEvent count to the few candidates out of ~200 navigable widgets.
    for (const auto& e : registry::bucket(registry::Kind::Menu)) {
        auto* w = e.handle.get();
        if (!w || s->widget_slot.read<ue::UObject*>(w)) continue;
        const auto vis = s->widget_visibility.read<std::uint8_t>(w); // ESlateVisibility: 0 Visible, 3/4 HitTestInvisible
        if (vis != 0 && vis != 3 && vis != 4) continue;
        ++c_menu_checks;
        reflect::Frame f(s->in_viewport);
        if (f.invoke(w) && f.ret_bool()) {
            g_menu_open = true;
            break;
        }
    }
    return g_menu_open;
}

bool display_player_ui() {
    const auto now = clock::now_us();
    if (now - g_display_at < 250'000) return g_display;
    g_display_at = now;
    const auto* s = schema();
    if (!s) return true;
    auto* sub = g_settings.get();
    if (!sub) {
        sub = world::find_instance(static_cast<ue::UClass*>(s->settings_class.get()));
        g_settings = ObjHandle::of(sub);
    }
    if (!sub) return g_display = true;
    reflect::Frame f(s->display_ui);
    g_display = !f.invoke(sub) || f.ret_bool();
    return g_display;
}

namespace {

// Direct projection from P1's camera cache (APlayerCameraManager.CameraCachePrivate.POV: Location, Rotation,
// FOV = horizontal, MaintainXFOV) with the overlay's back-buffer size. Audited once a second against the
// vanilla ProjectWorldToScreen for the first anchor; three audits off by more than 2 px → vanilla only.
struct Pov {
    reflect::PropRef pc_manager, cache, pov, location, rotation, fov;
    bool ok = false, tried = false;
};
Pov g_pov;
int g_bad_audits = 0;
bool g_direct_off = false;
std::int64_t g_audit_at = 0;
float g_audit_px = 0;
metrics::Counter& c_direct = metrics::counter("ui.project_direct");
metrics::Counter& c_vanilla = metrics::counter("ui.project_vanilla");

bool resolve_pov() {
    if (g_pov.tried) return g_pov.ok;
    g_pov.tried = true;
    reflect::Resolver pc("PlayerController", reflect::find_class(L"/Script/Engine.PlayerController"));
    g_pov.pc_manager = pc.prop(L"PlayerCameraManager", Kind::Object);
    reflect::Resolver cm("PlayerCameraManager", reflect::find_class(L"/Script/Engine.PlayerCameraManager"));
    g_pov.cache = cm.prop(L"CameraCachePrivate", Kind::Struct);
    reflect::Resolver ce("CameraCacheEntry", static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Engine.CameraCacheEntry")));
    g_pov.pov = ce.prop(L"POV", Kind::Struct);
    reflect::Resolver mv("MinimalViewInfo", static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Engine.MinimalViewInfo")));
    g_pov.location = mv.prop(L"Location", Kind::Struct, 12);
    g_pov.rotation = mv.prop(L"Rotation", Kind::Struct, 12);
    g_pov.fov = mv.prop(L"FOV", Kind::Float, 4);
    g_pov.ok = pc.ok() && cm.ok() && ce.ok() && mv.ok();
    if (!g_pov.ok) log::warn("HUD projection: camera cache unresolved, vanilla ProjectWorldToScreen only");
    return g_pov.ok;
}

std::optional<Screen> direct(const core::Vec3& at) {
    float w, h;
    auto* pc = world::p1_controller();
    auto* mgr = pc ? g_pov.pc_manager.object(pc) : nullptr;
    if (!mgr || !overlay::display_size(w, h)) return std::nullopt;
    const auto* pov = g_pov.cache.at<std::uint8_t>(mgr) + g_pov.pov.offset;
    const auto loc = g_pov.location.read<core::Vec3>(pov);
    const auto rot = g_pov.rotation.read<core::Vec3>(pov); // FRotator: Pitch, Yaw, Roll (degrees)
    const float fov = g_pov.fov.read<float>(pov);
    constexpr float kRad = 3.14159265358979f / 180.0f;
    const float sp = std::sin(rot.x * kRad), cp = std::cos(rot.x * kRad), sy = std::sin(rot.y * kRad), cy = std::cos(rot.y * kRad),
                sr = std::sin(rot.z * kRad), cr = std::cos(rot.z * kRad);
    const core::Vec3 d{at.x - loc.x, at.y - loc.y, at.z - loc.z};
    const float vx = d.x * cp * cy + d.y * cp * sy + d.z * sp;                                          // forward
    const float vy = d.x * (sr * sp * cy - cr * sy) + d.y * (sr * sp * sy + cr * cy) - d.z * sr * cp;     // right
    const float vz = -d.x * (cr * sp * cy + sr * sy) + d.y * (cy * sr - cr * sp * sy) + d.z * cr * cp;   // up
    if (vx <= 1.0f || fov <= 1.0f) return std::nullopt;
    const float t = std::tan(fov * 0.5f * kRad);
    return Screen{(1.0f + vy / (vx * t)) * 0.5f * w, (1.0f - vz * (w / h) / (vx * t)) * 0.5f * h};
}

std::optional<Screen> vanilla(const Schema& s, const core::Vec3& at) {
    auto* pc = world::p1_controller();
    if (!pc) return std::nullopt;
    ++c_vanilla;
    reflect::Frame f(s.project);
    f.arg(0, pc).arg(1, at).arg_bool(3, false);
    if (!f.invoke(s.gs.get()) || !f.ret_bool()) return std::nullopt;
    Screen out{};
    std::memcpy(&out, f.raw(2), sizeof(out));
    return out;
}

} // namespace

std::optional<Screen> project(const core::Vec3& at) {
    const auto* s = schema();
    if (!s) return std::nullopt;
    if (g_direct_off || !resolve_pov()) return vanilla(*s, at);
    const auto d = direct(at);
    if (const auto now = clock::now_us(); now >= g_audit_at) {
        g_audit_at = now + 1'000'000;
        const auto v = vanilla(*s, at);
        if (v && d) {
            g_audit_px = std::max(std::abs(v->x - d->x), std::abs(v->y - d->y));
            g_bad_audits = g_audit_px > 2.0f ? g_bad_audits + 1 : 0;
            if (g_bad_audits >= 3) {
                g_direct_off = true;
                log::warn("HUD projection: direct camera math off by {:.1f} px; using vanilla ProjectWorldToScreen", g_audit_px);
            }
        }
        if (g_direct_off || !d) return v;
    }
    if (d) ++c_direct;
    return d;
}

std::string projection_status() {
    return std::format("projection {} (last audit {:.2f} px, bad {}; direct {} / vanilla {})", g_direct_off ? "vanilla" : "direct", g_audit_px, g_bad_audits, c_direct.load(),
                       c_vanilla.load());
}

float special_cost() {
    if (g_cost >= 0) return g_cost;
    const auto* s = schema();
    if (!s) return 100;
    reflect::Frame f(s->special_cost);
    g_cost = f.invoke(s->logic.get()) ? static_cast<float>(f.ret<std::int32_t>()) : 100.0f;
    return g_cost;
}

void install() {
    lifecycle::on_teardown([] {
        g_holders = 0;
        g_we_paused = false;
        g_menu_open = false;
        g_menu_version = ~0ull;
        g_settings.reset();
        g_cost = -1;
    });
    // MenuOpenCache source: the registry keeps the Menu bucket only while seats exist (G0 without them).
    frame::add_phase("party.ui.menuwatch", [](const frame::Tick&) { registry::want_menu(party::count() > 0); },
                     [] { return (party::count() > 0) != registry::menu_wanted(); }, 0);
}

} // namespace elmp::party::ui
