#include "coop/presentation_policy.h"

#include "engine/handle.h"
#include "engine/reflect.h"
#include "guard/guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <vector>

namespace elmp::coop::presentation_policy {
namespace {

struct Schema {
    ObjHandle actor_class, pawn_class, component_class;
    reflect::PropRef controller;
    reflect::FuncRef components, actor_tick, actor_collision, component_tick;

    bool live() const {
        return actor_class.valid() && pawn_class.valid() && component_class.valid() && components.handle.valid() && actor_tick.handle.valid() &&
               actor_collision.handle.valid() && component_tick.handle.valid();
    }
};

struct RootFactor {
    ObjHandle component, type;
    reflect::PropRef property;
    float original = 0;
};

struct Entry {
    ObjHandle actor, controller;
    std::vector<RootFactor> roots;
    std::vector<ObjHandle> skeletons;
    bool ready = false;
};

// Invalidated by component/actor serial changes, release(), reset(), or schema class expiry.
// describe() audits all live saved factors and reports drift without silently correcting it.
std::optional<Schema> g_schema;
std::unordered_map<ObjHandle, Entry> g_entries;
// Native/derived component class handles prevent stale UClass pointer reuse across worlds.
std::unordered_map<ObjHandle, reflect::FuncRef> g_battle_calls;
std::string g_failure;
std::uint64_t g_applied = 0, g_repeated = 0, g_failed = 0, g_root_zeroed = 0, g_restored = 0, g_restore_failed = 0, g_expired = 0;
std::uint64_t g_engaged = 0, g_engage_failed = 0;

const Schema* schema() {
    if (g_schema && g_schema->live()) return &*g_schema;
    g_schema.reset();
    Schema s;
    auto* actor = reflect::find_class(L"/Script/Engine.Actor");
    auto* pawn = reflect::find_class(L"/Script/Engine.Pawn");
    auto* component = reflect::find_class(L"/Script/Engine.ActorComponent");
    reflect::Resolver a("presentation Actor", actor), p("presentation Pawn", pawn), c("presentation ActorComponent", component);
    s.components = a.func(L"K2_GetComponentsByClass", 1);
    s.actor_tick = a.func(L"SetActorTickEnabled", 1);
    s.actor_collision = a.func(L"SetActorEnableCollision", 1);
    s.controller = p.prop(L"Controller", reflect::Kind::Object);
    s.component_tick = c.func(L"SetComponentTickEnabled", 1);
    if (!a.ok() || !p.ok() || !c.ok()) {
        g_failure = a.failure() + p.failure() + c.failure();
        return nullptr; // Native schema is retried; a cold lookup failure is not permanent.
    }
    s.actor_class = ObjHandle::of(actor);
    s.pawn_class = ObjHandle::of(pawn);
    s.component_class = ObjHandle::of(component);
    g_schema = std::move(s);
    return &*g_schema;
}

bool set_bool(const reflect::FuncRef& fn, ue::UObject* object, bool on) {
    reflect::Frame f(fn);
    f.arg_bool(0, on);
    return f.invoke(object);
}

bool components(const Schema& s, ue::UObject* actor, std::vector<ObjHandle>& out) {
    reflect::Frame f(s.components);
    f.arg(0, s.component_class.get());
    if (!f.invoke(actor)) return false;
    const auto* ar = static_cast<const reflect::RawArray*>(f.raw(s.components.ret));
    if (!ar || ar->num < 0 || ar->num > 512 || ar->max < ar->num || (ar->num && !ar->data)) return false;
    const auto* data = reinterpret_cast<ue::UObject* const*>(ar->data);
    for (int i = 0; i < ar->num; ++i)
        if (ue::alive(data[i])) out.push_back(ObjHandle::of(data[i]));
    return true;
}

bool named_base(ue::UObject* object, std::wstring_view wanted) {
    // Cold apply/engage path only. Supports inherited component classes without an unsafe raw class cache.
    for (auto* type = object ? object->GetClassPrivate() : nullptr; type; type = static_cast<ue::UClass*>(type->GetSuperStruct()))
        if (reflect::same_name(ue::name_of(type), wanted)) return true;
    return false;
}

bool visual(std::wstring_view name) {
    return name == L"SpineSkeletonAnimationComponent" || name == L"SpineSkeletonAnimationExComponent" || name == L"SpineSkeletonRendererComponent" || name == L"SpineBoneFollowerComponent" ||
           name == L"SpineBoneComponent" || name == L"ParticleSystemComponent" || name == L"WidgetComponent" || name == L"BattleColorComponent";
}

bool close_actor(const Schema& s, ue::UObject* actor) {
    guard::mark(actor);
    const bool tick = set_bool(s.actor_tick, actor, false);
    const bool collision = set_bool(s.actor_collision, actor, false);
    return tick && collision;
}

void restore(Entry& entry) {
    // The bone-update delegate applies root motion independently of the logic component's tick.
    // Stop its producer before returning the saved factor, even when release precedes a world reload.
    if (const auto* s = schema())
        for (const auto& handle : entry.skeletons)
            if (auto* component = handle.get(); component && !set_bool(s->component_tick, component, false)) ++g_restore_failed;
    for (const auto& root : entry.roots) {
        auto* component = root.component.get();
        if (!component || root.type.get() != component->GetClassPrivate()) continue;
        std::memcpy(root.property.at<float>(component), &root.original, sizeof(float));
        if (root.property.read<float>(component) == root.original) ++g_restored;
        else ++g_restore_failed;
    }
    // Lifecycle owner releases guard marks/reloads the world. Never re-enable actor/component ticks here.
}

bool zero_root(Entry& entry, ue::UObject* component) {
    const auto handle = ObjHandle::of(component);
    auto found = std::find_if(entry.roots.begin(), entry.roots.end(), [&](const RootFactor& r) { return r.component == handle; });
    if (found == entry.roots.end()) {
        reflect::Resolver r("presentation SpineAnimation", component->GetClassPrivate());
        const auto property = r.prop(L"RootMotionFactor", reflect::Kind::Float, sizeof(float));
        if (!r.ok()) {
            g_failure = r.failure();
            return false;
        }
        const float original = property.read<float>(component);
        if (!std::isfinite(original)) {
            g_failure = "RootMotionFactor is not finite";
            return false;
        }
        entry.roots.push_back({handle, ObjHandle::of(component->GetClassPrivate()), property, original});
        found = std::prev(entry.roots.end());
    }
    const float zero = 0;
    std::memcpy(found->property.at<float>(component), &zero, sizeof(zero));
    if (found->property.read<float>(component) != 0.0f) {
        g_failure = "RootMotionFactor did not read back as zero";
        return false;
    }
    ++g_root_zeroed;
    return true;
}

void prune() {
    std::erase_if(g_entries, [](const auto& pair) {
        if (pair.first.valid()) return false;
        ++g_expired;
        return true;
    });
    std::erase_if(g_battle_calls, [](const auto& pair) { return !pair.first.valid() || !pair.second.handle.valid(); });
}

} // namespace

bool apply(ue::UObject* actor) {
    ++g_applied;
    if (!ue::on_game_thread() || !ue::alive(actor)) {
        ++g_failed;
        g_failure = "apply requires a live actor on the game thread";
        return false;
    }
    guard::mark(actor);
    const auto* s = schema();
    if (!s || !actor->IsA(static_cast<ue::UClass*>(s->actor_class.get()))) {
        // Even a missing component enumeration API must not leave an already spawned replica's
        // Actor tick running. These independent Actor entries do not depend on the failed schema.
        reflect::Resolver a("presentation fallback", reflect::find_class(L"/Script/Engine.Actor"));
        const auto tick = a.func(L"SetActorTickEnabled", 1);
        const auto collision = a.func(L"SetActorEnableCollision", 1);
        if (tick.ok()) set_bool(tick, actor, false);
        if (collision.ok()) set_bool(collision, actor, false);
        ++g_failed;
        if (s) g_failure = "apply requires an Actor";
        return false;
    }
    prune();
    auto [it, fresh] = g_entries.try_emplace(ObjHandle::of(actor));
    auto& entry = it->second;
    if (!fresh) ++g_repeated;
    entry.actor = it->first;
    entry.ready = false;
    bool ok = close_actor(*s, actor);
    if (actor->IsA(static_cast<ue::UClass*>(s->pawn_class.get()))) {
        auto* controller = s->controller.object(actor);
        if (controller) {
            entry.controller = ObjHandle::of(controller);
            ok = close_actor(*s, controller) && ok;
            std::vector<ObjHandle> controller_items;
            if (!components(*s, controller, controller_items)) {
                ok = false;
                g_failure = "could not enumerate controller components";
            }
            for (const auto& handle : controller_items)
                if (auto* component = handle.get(); component && !set_bool(s->component_tick, component, false)) {
                    ok = false;
                    g_failure = "could not close a controller component tick";
                }
        }
    }
    std::vector<ObjHandle> items;
    const bool listed = components(*s, actor, items);
    ok = listed && ok && guard::armed();
    if (!listed) g_failure = "could not enumerate actor components";
    else if (!guard::armed()) g_failure = "presentation guard is not armed";
    else if (!ok) g_failure = "could not close actor/controller tick or collision";

    // First disable autonomous components and close root motion. No skeleton tick is opened until
    // every SpineAnimation root-motion factor has been written and read back successfully.
    entry.skeletons.clear();
    for (const auto& handle : items) {
        auto* component = handle.get();
        if (!component) continue;
        const auto name = ue::class_name_of(component);
        if (name == L"SpineSkeletonAnimationComponent" || name == L"SpineSkeletonAnimationExComponent") entry.skeletons.push_back(handle);
        if (named_base(component, L"SpineAnimationComponent")) {
            if (!set_bool(s->component_tick, component, false)) {
                ok = false;
                g_failure = "could not close SpineAnimation logic tick";
            }
            ok = zero_root(entry, component) && ok;
        } else if (!visual(name) && !named_base(component, L"ColorComponent")) {
            if (!set_bool(s->component_tick, component, false)) {
                ok = false;
                g_failure = "could not close a logic component tick";
            }
        }
    }
    for (const auto& handle : items) {
        auto* component = handle.get();
        if (!component || !visual(ue::class_name_of(component))) continue;
        if (!set_bool(s->component_tick, component, ok)) {
            ok = false;
            g_failure = "could not set a presentation component tick";
        }
    }
    if (!ok) {
        // A later presentation failure must not leave earlier skeleton components ticking.
        for (const auto& handle : items)
            if (auto* component = handle.get(); component && visual(ue::class_name_of(component))) set_bool(s->component_tick, component, false);
        ++g_failed;
        return false;
    }
    entry.ready = true;
    return true;
}

void release(ue::UObject* actor) {
    if (!ue::on_game_thread() || !ue::alive(actor)) return;
    const auto it = g_entries.find(ObjHandle::of(actor));
    if (it == g_entries.end()) return;
    restore(it->second);
    g_entries.erase(it);
}

void reset() {
    if (!ue::on_game_thread()) return;
    for (auto& [handle, entry] : g_entries) restore(entry);
    g_entries.clear();
    g_battle_calls.clear();
    g_schema.reset();
}

bool engage(ue::UObject* actor, bool on) {
    if (!ue::on_game_thread() || !ue::alive(actor)) return ++g_engage_failed, false;
    const auto* s = schema();
    std::vector<ObjHandle> items;
    if (!s || !actor->IsA(static_cast<ue::UClass*>(s->actor_class.get())) || !components(*s, actor, items)) return ++g_engage_failed, false;
    for (const auto& handle : items) {
        auto* component = handle.get();
        if (!component || !named_base(component, L"BattleColorComponent")) continue;
        const auto type = ObjHandle::of(component->GetClassPrivate());
        auto found = g_battle_calls.find(type);
        if (found == g_battle_calls.end() || !found->second.handle.valid()) {
            reflect::Resolver r("presentation BattleColor", component->GetClassPrivate());
            auto call = r.func(L"SetBattleColorEnabled", 1);
            if (!r.ok()) {
                g_failure = r.failure();
                return ++g_engage_failed, false;
            }
            found = g_battle_calls.insert_or_assign(type, std::move(call)).first;
        }
        if (!set_bool(found->second, component, on)) return ++g_engage_failed, false;
        ++g_engaged;
    }
    return true;
}

std::string describe() {
    if (!ue::on_game_thread()) return "presentation-policy: game thread required\n";
    prune();
    std::size_t ready = 0, roots = 0, drift = 0;
    for (const auto& [handle, entry] : g_entries) {
        ready += entry.ready;
        for (const auto& root : entry.roots)
            if (auto* component = root.component.get(); component && root.type.get() == component->GetClassPrivate()) {
                ++roots;
                if (root.property.read<float>(component) != 0.0f) ++drift;
            }
    }
    return std::format("presentation-policy: actors={} ready={} roots={} rootDrift={} apply={} repeat={} failed={} rootZeroed={} restored={} restoreFailed={} expired={} "
                       "engage={} engageFailed={} last='{}'\n",
                       g_entries.size(), ready, roots, drift, g_applied, g_repeated, g_failed, g_root_zeroed, g_restored, g_restore_failed, g_expired, g_engaged,
                       g_engage_failed, g_failure);
}

} // namespace elmp::coop::presentation_policy
