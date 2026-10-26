#include "party/camera_sync.h"

#include "party/seat.h"

#include "camera/framing.h"
#include "core/player.h"

#include "engine/frame.h"
#include "engine/handle.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/log.h"
#include "base/metrics.h"
#include "base/settings.h"

#include <algorithm>
#include <format>
#include <optional>
#include <vector>

namespace elmp::party::camera_sync {

using reflect::Kind;

namespace {

struct Schema {
    reflect::PropRef pc_manager, registered_camera, allies;
    reflect::FuncRef reg, unreg;
    ObjHandle manager_class;
};
std::optional<Schema> g_schema;
std::string g_schema_failure;

ObjHandle g_camera;
std::vector<ObjHandle> g_registered; // controllers we registered on g_camera
bool g_dirty = true;                  // framing settings / seat presence changed
bool g_framing = false;
std::string g_note;
metrics::Counter& c_register = metrics::counter("camera.register_calls");
metrics::Counter& c_rebind = metrics::counter("camera.rebinds");

const Schema* schema() {
    if (g_schema || !g_schema_failure.empty()) return g_schema ? &*g_schema : nullptr;
    Schema s;
    reflect::Resolver pc("PlayerController", reflect::find_class(L"/Script/Engine.PlayerController"));
    s.pc_manager = pc.prop(L"PlayerCameraManager", Kind::Object);
    auto* manager_class = reflect::find_class(L"/Script/Zenith.PlayerCameraManagerZenithBase");
    s.manager_class = ObjHandle::of(manager_class);
    reflect::Resolver mgr("PlayerCameraManagerZenithBase", manager_class);
    s.registered_camera = mgr.prop(L"RegisteredGameplayCamera", Kind::Weak, 8);
    reflect::Resolver cam("GameplayCamera", reflect::find_class(L"/Script/Zenith.GameplayCamera"));
    s.allies = cam.prop(L"AllyAIControllerTargets", Kind::Array, 16);
    s.reg = cam.func(L"RegisterAllyController", 1);
    s.unreg = cam.func(L"UnregisterAllyController", 1);
    for (const auto* r : {&pc, &mgr, &cam})
        if (!r->ok() && g_schema_failure.empty()) g_schema_failure = r->failure();
    if (!g_schema_failure.empty()) {
        log::error("camera sync disabled: {}", g_schema_failure);
        return nullptr;
    }
    g_schema = s;
    return &*g_schema;
}

ue::UObject* camera(const Schema& s) {
    auto* pc = world::p1_controller();
    auto* mgr = pc ? s.pc_manager.object(pc) : nullptr;
    auto* zenith = static_cast<ue::UClass*>(s.manager_class.get());
    if (!mgr || !zenith || !mgr->GetClassPrivate()->IsChildOf(zenith)) return nullptr;
    auto* cam = s.registered_camera.at<RC::Unreal::FWeakObjectPtr>(mgr)->Get();
    return ue::alive(cam) ? cam : nullptr;
}

bool listed(const Schema& s, ue::UObject* cam, const ue::UObject* pc) {
    const auto& arr = reflect::array_at(cam, s.allies);
    const auto* items = reinterpret_cast<const RC::Unreal::FWeakObjectPtr*>(arr.data);
    for (int i = 0; i < arr.num; ++i)
        if (items[i].Get() == pc) return true;
    return false;
}

void call(const reflect::FuncRef& f, ue::UObject* cam, ue::UObject* pc) {
    reflect::Frame(f).arg(0, pc).invoke(cam);
    ++c_register;
}

bool wanted() { return party::count() > 0 && settings::get_bool("cameraFraming"); }

void push_framing(bool want) {
    const camera::Margins m{static_cast<float>(settings::get_number("cameraSideMargin")), static_cast<float>(settings::get_number("cameraTopMargin")),
                            static_cast<float>(settings::get_number("cameraBottomMargin")), static_cast<float>(settings::get_number("cameraMaxDistanceScale"))};
    const auto err = camera::configure(want, m);
    if (!err.empty() && g_note != err) log::warn("camera framing unavailable: {}", err);
    g_note = err;
    g_framing = want && err.empty();
    g_dirty = false;
}

void step(const frame::Tick&) {
    if (const bool want = wanted(); g_dirty || want != g_framing) push_framing(want);
    const auto* s = schema();
    if (!s) return;
    auto* cam = camera(*s);
    if (ObjHandle::of(cam) != g_camera) { // camera recreated (map switch / reload): our registrations died with it
        if (!g_camera.empty()) ++c_rebind;
        g_camera = ObjHandle::of(cam);
        g_registered.clear();
    }
    if (!cam) return;
    std::vector<ObjHandle> want;
    for (const auto& seat : party::seats())
        if (auto* pc = core::controller(seat.slot); pc && seat.source != party::Source::Network) want.push_back(ObjHandle::of(pc)); // remote guests have their own screen
    for (const auto& h : g_registered) {
        if (std::find(want.begin(), want.end(), h) != want.end()) continue;
        if (auto* pc = h.get()) call(s->unreg, cam, pc);
    }
    for (const auto& h : want)
        if (auto* pc = h.get(); pc && !listed(*s, cam, pc)) call(s->reg, cam, pc);
    g_registered = std::move(want);
}

} // namespace

void left(std::int64_t slot) {
    const auto h = ObjHandle::of(core::controller(slot));
    if (const auto it = std::find(g_registered.begin(), g_registered.end(), h); it != g_registered.end()) {
        g_registered.erase(it);
        const auto* s = schema();
        auto* cam = s ? camera(*s) : nullptr;
        if (auto* pc = h.get(); cam && pc) call(s->unreg, cam, pc);
    }
    if (party::count() == 0 && g_framing) push_framing(false);
}

std::string describe() {
    const auto st = camera::status();
    const auto* s = schema();
    auto* cam = s ? camera(*s) : nullptr;
    int allies = cam ? reflect::array_at(cam, s->allies).num : -1;
    return std::format("framing={} hooked={} widened={} camera={} allies={} registered={} register_calls={} rebinds={}{}\n", st.enabled, st.hooked, st.widened,
                       cam ? ue::utf8_name(cam) : "-", allies, g_registered.size(), c_register.load(), c_rebind.load(),
                       g_note.empty() ? (s ? "" : " schema: " + g_schema_failure) : " (" + g_note + ")");
}

void install() {
    frame::add_phase("party.camera", step, [] { return party::count() > 0 || !g_registered.empty() || g_framing; }, 500);
    settings::on_change([](const std::string& key) {
        if (key.starts_with("camera")) g_dirty = true;
    });
    lifecycle::on_teardown([] {
        g_camera.reset();
        g_registered.clear();
        if (g_framing) push_framing(false); // seats are dropped with the world
        g_dirty = true;
    });
}

} // namespace elmp::party::camera_sync
