#include "camera/framing.h"

#include "engine/reflect.h"

#include "base/hook.h"
#include "base/log.h"

#include <Unreal/Core/Containers/Array.hpp>

#include <algorithm>
#include <optional>

namespace elmp::camera {

using reflect::Kind;
using namespace RC::Unreal;

namespace {

struct Layout {
    std::int32_t players, allies, max_distance, offset_z;
};

using UpdateFn = std::uint64_t(__fastcall*)(UObject*, float, bool);
using BoundsFn = std::uint8_t(__fastcall*)(UObject*, float*, float*);

UpdateFn g_update{};
BoundsFn g_bounds{};
std::optional<Layout> g_layout;
Margins g_margins;
bool g_enabled = false;
UObject* g_active = nullptr; // camera being widened (non-null only inside its Update call)
std::uint64_t g_widened = 0;
std::string g_failure;

std::optional<Layout> resolve() {
    reflect::Resolver cam("GameplayCamera", reflect::find_class(L"/Script/Zenith.GameplayCamera"));
    const auto players = cam.prop(L"PlayerControllerTargets", Kind::Array, 16);
    const auto allies = cam.prop(L"AllyAIControllerTargets", Kind::Array, 16);
    const auto current = cam.prop(L"CurrentSettings", Kind::Struct);
    reflect::Resolver set("GameplayCameraSettings", static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.GameplayCameraSettings")));
    const auto max_distance = set.prop(L"MaxDistance", Kind::Float, 4);
    const auto offset = set.prop(L"WorldSpaceLocationOffset", Kind::Struct, 12);
    if (!cam.ok() || !set.ok()) {
        g_failure = !cam.ok() ? cam.failure() : set.failure();
        return std::nullopt;
    }
    return Layout{players.offset, allies.offset, current.offset + max_distance.offset, current.offset + offset.offset + 8};
}

template <class T>
T& at(UObject* o, std::int32_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<std::uint8_t*>(o) + offset);
}

int friendly_targets(UObject* camera) {
    int n = 0;
    for (const auto offset : {g_layout->players, g_layout->allies})
        for (auto& target : at<TArray<FWeakObjectPtr>>(camera, offset))
            if (target.Get()) ++n;
    return n;
}

// Vanilla: distance = box fit, clamped to [MinDistance, MaxDistance] (≤ 0 = unlimited).
std::uint64_t __fastcall update(UObject* camera, float dt, bool lag) {
    if (!g_enabled || g_active || !ue::on_game_thread() || friendly_targets(camera) < 2) return g_update(camera, dt, lag);
    auto& max = at<float>(camera, g_layout->max_distance);
    const float original = max, widened = original <= 0.0f ? original : original * g_margins.max_distance_scale;
    max = widened;
    g_active = camera;
    const auto r = g_update(camera, dt, lag);
    g_active = nullptr;
    if (max == widened) max = original; // vanilla SetSettings during Update wins
    ++g_widened;
    return r;
}

// Vanilla box = every target ± 2 × bounds sphere radius (Y / Z) with no slack; the vertical view offset
// shifts the box, so the opposite side gets that much extra to keep the original box on screen.
std::uint8_t __fastcall bounds(UObject* camera, float* min, float* max) {
    const auto found = g_bounds(camera, min, max);
    if (!found || camera != g_active) return found;
    const float oz = at<float>(camera, g_layout->offset_z);
    min[1] -= g_margins.side;
    max[1] += g_margins.side;
    min[2] -= g_margins.bottom + std::max(oz, 0.0f);
    max[2] += g_margins.top + std::max(-oz, 0.0f);
    return found;
}

} // namespace

std::string configure(bool enabled, const Margins& margins) {
    g_margins = margins;
    if (enabled && !g_update) {
        if (!g_failure.empty()) { // refusals are final for this process (fail closed, reported once)
            g_enabled = false;
            return g_failure;
        }
        if (!g_layout) g_layout = resolve();
        if (!g_layout) {
            g_failure = "camera layout: " + g_failure;
            g_enabled = false;
            return g_failure;
        }
        if (!hook::install_manifest("GameplayCameraTargetBounds", reinterpret_cast<void*>(&bounds), reinterpret_cast<void**>(&g_bounds)) ||
            !hook::install_manifest("GameplayCameraUpdate", reinterpret_cast<void*>(&update), reinterpret_cast<void**>(&g_update))) {
            g_update = nullptr; // bounds alone is inert (only acts while g_active is set by update)
            g_failure = "camera hooks refused (see preflight)";
            g_enabled = false;
            return g_failure;
        }
        log::info("camera: framing detours armed");
    }
    g_enabled = enabled;
    return {};
}

Status status() { return {g_enabled, g_update != nullptr, g_widened, g_failure}; }

} // namespace elmp::camera
