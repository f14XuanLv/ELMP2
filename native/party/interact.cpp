#include "party/interact.h"

#include "core/player.h"

#include "engine/reflect.h"
#include "engine/registry.h"

#include "base/log.h"
#include "base/metrics.h"
#include "base/preflight.h"

#include <format>
#include <optional>

namespace elmp::party::interact {

namespace {

using reflect::Kind;

struct Schema {
    reflect::FuncRef interacting; // AInteractable::GetInteractingController()
    reflect::FuncRef input_type;  // AInteractable::GetInteractableInputType() (EInteractableInputType)
};
std::optional<Schema> g_s;
std::string g_failure;

using PressFn = void(__fastcall*)(ue::UObject*);
PressFn g_press = nullptr;

metrics::Counter& c_delivered = metrics::counter("interact.delivered");
metrics::Counter& c_skipped_up = metrics::counter("interact.up_type");
std::string g_last;

const Schema* schema() {
    if (g_s || !g_failure.empty()) return g_s ? &*g_s : nullptr;
    Schema s;
    reflect::Resolver r("Interactable", reflect::find_class(L"/Script/Zenith.Interactable"));
    s.interacting = r.func(L"GetInteractingController", 0);
    s.input_type = r.func(L"GetInteractableInputType", 0);
    g_press = reinterpret_cast<PressFn>(preflight::address("InteractablePressInteract"));
    if (!r.ok()) g_failure = r.failure();
    else if (!g_press) g_failure = "InteractablePressInteract not verified by preflight";
    if (!g_failure.empty()) {
        log::error("PX interaction unavailable: {}", g_failure);
        return nullptr;
    }
    g_s = s;
    return &*g_s;
}

} // namespace

std::string press(std::int64_t slot) {
    auto* pc = core::controller(slot);
    if (!pc) return {};
    const auto* s = schema();
    if (!s) return {};
    for (const auto& e : registry::bucket(registry::Kind::Interactable)) {
        auto* it = e.handle.get();
        if (!it) continue;
        reflect::Frame who(s->interacting);
        if (!who.invoke(it) || who.ret<ue::UObject*>() != pc) continue;
        reflect::Frame type(s->input_type);
        if (!type.invoke(it)) continue;
        const auto kind = type.ret<std::uint8_t>();
        const auto name = ue::utf8(e.name);
        if (kind != 0) { // UP / DOWN: the seat's direction input drives the vanilla command
            ++c_skipped_up;
            return g_last = std::format("P{} {}: direction-type interactable (press Up/Down)", slot, name);
        }
        g_press(it);
        ++c_delivered;
        log::info("party: P{} interact -> {}", slot, name);
        return g_last = std::format("P{} interact -> {}", slot, name);
    }
    return {};
}

std::string status() {
    return std::format("interact: {} delivered={} directionType={} last='{}'", g_failure.empty() ? "ok" : "unavailable: " + g_failure, c_delivered.load(),
                       c_skipped_up.load(), g_last);
}

} // namespace elmp::party::interact
