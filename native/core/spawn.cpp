#include "core/spawn.h"

#include "core/schema.h"
#include "core/skin.h"
#include "core/vitals.h"

#include "input/virtual_pad.h"

#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "base/events.h"
#include "base/log.h"
#include "base/metrics.h"

#include <cmath>
#include <format>

namespace elmp::core::spawn {

using namespace RC::Unreal;

namespace {

// FTransform (UE 4.26, float): FQuat, FVector + pad, FVector + pad; 16-byte aligned.
struct alignas(16) Transform {
    float q[4];
    float t[3];
    float pad0;
    float s[3];
    float pad1;
};
static_assert(sizeof(Transform) == 48);

metrics::Counter& c_spawns = metrics::counter("core.spawns");
metrics::Counter& c_respawns = metrics::counter("core.respawns");
metrics::Counter& c_rollbacks = metrics::counter("core.spawn_rollbacks");
metrics::Counter& c_orphans = metrics::counter("core.orphans_removed");

ue::UObject* spawn_actor(const schema::Script& s, ue::UObject* cls, Vec3 at, float yaw) {
    auto* w = world::world();
    auto* cdo = s.gs_cdo.get();
    if (!w || !cdo || !cls) return nullptr;
    const float half = yaw * 3.14159265358979f / 360.0f; // FRotator(0, yaw, 0) → quaternion about Z
    const Transform x{{0.0f, 0.0f, std::sin(half), std::cos(half)}, {at.x, at.y, at.z}, 0.0f, {1.0f, 1.0f, 1.0f}, 0.0f};
    reflect::Frame begin(s.gs_begin_spawn);
    begin.arg(0, w).arg(1, cls).arg(2, x).arg(3, std::uint8_t{0}).arg(4, static_cast<ue::UObject*>(nullptr));
    if (!begin.invoke(cdo)) return nullptr;
    auto* actor = begin.ret<ue::UObject*>();
    if (!ue::alive(actor)) return nullptr;
    reflect::Frame finish(s.gs_finish_spawn);
    finish.arg(0, actor).arg(1, x);
    finish.invoke(cdo);
    return ue::alive(actor) ? actor : nullptr;
}

// PlayerCameraManagerZenith would BeginPlay with a null PCOwner inside SpawnActor and crash: swap the
// class on the PC CDO for the engine base class during the spawn, restore right after.
ue::UObject* new_controller(const schema::Script& s, const schema::Blueprint& b, Vec3 at, float yaw) {
    auto* cls = static_cast<UClass*>(b.pc_class.get());
    auto* engine_cam = s.engine_camera_manager_class.get();
    if (!cls || !engine_cam) return nullptr;
    UObject* cdo = cls->GetClassDefaultObject();
    if (!ue::alive(cdo)) return nullptr;
    auto* slot = s.pc_camera_class.at<ue::UObject*>(cdo);
    ue::UObject* original = *slot;
    *slot = engine_cam;
    ue::UObject* pc = nullptr;
    try {
        pc = spawn_actor(s, cls, at, yaw);
    } catch (...) {
        *slot = original;
        throw;
    }
    *slot = original;
    return pc;
}

bool destroy(const schema::Script& s, ue::UObject* actor) {
    for (int i = 0; i < 2; ++i) {
        if (!ue::alive(actor) || s.actor_destroying.read_bool(actor)) return true;
        reflect::Frame(s.actor_destroy).invoke(actor);
        if (!ue::alive(actor) || s.actor_destroying.read_bool(actor)) return true;
    }
    return false;
}

// A pawn tagged for this slot without a controller (e.g. a body left by an interrupted removal).
// Uses the registry's Lily bucket — no object-array scan.
int sweep_orphans(const schema::Script& s, std::int64_t slot) {
    int removed = 0;
    std::vector<ue::UObject*> victims;
    for (const auto& e : registry::bucket(registry::Kind::Lily)) {
        auto* p = e.handle.get();
        if (!p || s.actor_destroying.read_bool(p) || s.pawn_controller.object(p)) continue;
        if (slot_of(p) == slot) victims.push_back(p);
    }
    for (auto* p : victims)
        if (destroy(s, p)) ++removed;
    c_orphans += static_cast<std::uint64_t>(removed);
    return removed;
}

std::optional<Vec3> default_location() {
    const auto* s = schema::script();
    auto* p1 = world::p1_pawn();
    if (!s || !p1) return std::nullopt;
    reflect::Frame f(s->actor_location);
    if (!f.invoke(p1)) return std::nullopt;
    auto l = f.ret<Vec3>();
    l.y += 120.0f;
    l.z += 10.0f;
    return l;
}

std::string ready(const schema::Script*& s, const schema::Blueprint*& b) {
    if (lifecycle::state() != lifecycle::State::Settled) return std::format("world not settled ({})", lifecycle::name(lifecycle::state()));
    s = schema::script();
    if (!s) return "core schema unresolved: " + schema::status();
    b = schema::blueprint();
    if (!b) return "core schema unresolved: " + schema::status();
    return {};
}

std::string do_spawn(std::int64_t slot, const Options& opt, bool respawning) {
    const schema::Script* s = nullptr;
    const schema::Blueprint* b = nullptr;
    if (auto err = ready(s, b); !err.empty()) return err;
    if (slot < 2) return "spawn requires slot >= 2";
    if (opt.yaw != 90.0f && opt.yaw != -90.0f) return "yaw must be 90 or -90";
    if (!skin::valid(opt.skin)) return std::format("skin must be 1..{}", skin::kCount);
    if (!leases().owner_of(slot)) return std::format("slot {} is not leased", slot);
    const auto at = opt.location ? opt.location : default_location();
    if (!at) return "no location given and no P1 pawn to spawn next to";

    auto* pc = controller(slot);
    if (pc && pawn(slot)) return std::format("slot {} already has a pawn", slot);
    if (!pc) {
        pc = new_controller(*s, *b, *at, opt.yaw);
        if (!pc) return "spawn controller failed";
        tag(pc, slot);
        remember(slot, pc);
    }
    sweep_orphans(*s, slot);

    const auto rollback = [&](std::string why) {
        ++c_rollbacks;
        despawn(slot, true);
        return why;
    };
    auto* p = spawn_actor(*s, b->pawn_class.get(), *at, opt.yaw);
    if (!p) return rollback("spawn pawn failed");
    tag(p, slot);
    if (auto err = skin::apply(p, opt.skin); !err.empty()) return rollback("skin: " + err);
    reflect::Frame possess(s->controller_possess);
    possess.arg(0, p);
    if (!possess.invoke(pc)) return rollback("Possess failed");
    reflect::Frame disable(s->actor_disable_input);
    disable.arg(0, pc);
    disable.invoke(p);
    // During a map switch Possess may not have pointed pawn.Controller back yet (v1 2026-10-09):
    // report it; the caller retries the whole spawn.
    if (s->pawn_controller.object(p) != pc || s->controller_pawn.object(pc) != p) return rollback("possess not effective yet");
    set_pawn(slot, p);
    if (auto err = input::pad::attach(slot, p, pc); !err.empty()) return rollback("pad: " + err);
    if (respawning) {
        if (auto* equip = s->zpc_spirit_equip.object(pc)) reflect::Frame(s->se_reequip).invoke(equip);
    }
    ++(respawning ? c_respawns : c_spawns);
    events::bus().push(respawning ? events::Type::Revived : events::Type::SeatJoin, static_cast<std::int32_t>(slot));
    return {};
}

} // namespace

std::string spawn(std::int64_t slot, const Options& opt) { return do_spawn(slot, opt, false); }

std::string despawn(std::int64_t slot, bool keep_equipment) {
    const auto* s = schema::script();
    if (!s) return "core schema unresolved";
    input::pad::detach(slot);
    auto* pc = controller(slot);
    if (!pc) return {};
    auto* p = s->controller_pawn.object(pc);
    if (!keep_equipment) {
        if (auto* equip = s->zpc_spirit_equip.object(pc)) reflect::Frame(s->se_unequip_all).invoke(equip);
    }
    reflect::Frame(s->controller_unpossess).invoke(pc);
    set_pawn(slot, nullptr);
    if (p && !destroy(*s, p)) return std::format("failed to destroy pawn of slot {}", slot);
    if (!keep_equipment) events::bus().push(events::Type::SeatLeave, static_cast<std::int32_t>(slot));
    return {};
}

std::string respawn(std::int64_t slot, const Options& opt) {
    if (auto err = despawn(slot, true); !err.empty()) return err;
    if (auto err = do_spawn(slot, opt, true); !err.empty()) return err;
    return vitals::restore(slot);
}

} // namespace elmp::core::spawn
