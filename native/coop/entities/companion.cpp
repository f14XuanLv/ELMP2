#include "coop/entities/companion.h"

#include "coop/entities/clearable.h"
#include "coop/entities/spawn.h"
#include "coop/entity.h"
#include "coop/presentation.h"
#include "coop/presentation_policy.h"
#include "coop/visual_state.h"
#include "core/player.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"
#include "guard/guard.h"
#include "party/seat.h"
#include "base/log.h"

#include <cmath>
#include <array>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>

namespace elmp::coop::entities {
namespace {

// Exact output bits, never inferred from IsActive: native SetActive also changes renderer/AI state.
constexpr std::uint8_t kHidden = 1, kActive = 2, kRendererHidden = 4, kRendererVisible = 8;
constexpr std::uint8_t kFxExists = 16, kFxActive = 32, kFxVisible = 64, kFxHidden = 128;

struct Schema {
    ObjHandle actor, pawn, companion, ai, scene, component, spine, particle;
    reflect::PropRef owner, hidden, controller, renderer, skeleton, scene_hidden, visible, active;
    reflect::FuncRef is_active, get_controller, leader, hide, set_hidden, set_visible, activate, deactivate, scale_x, set_scale_x;
    reflect::FuncRef get_rotation, set_rotation;
    bool live() const {
        return actor.valid() && pawn.valid() && companion.valid() && ai.valid() && scene.valid() && component.valid() && spine.valid() && particle.valid() &&
               is_active.handle.valid() && get_controller.handle.valid() && leader.handle.valid() && hide.handle.valid() &&
               set_hidden.handle.valid() && set_visible.handle.valid() && activate.handle.valid() && deactivate.handle.valid() && scale_x.handle.valid() && set_scale_x.handle.valid() &&
               get_rotation.handle.valid() && set_rotation.handle.valid();
    }
};
std::optional<Schema> g_schema;
std::string g_failure;

const Schema* schema() {
    if (g_schema && g_schema->live()) return &*g_schema;
    g_schema.reset();
    Schema s;
    auto* actor = reflect::find_class(L"/Script/Engine.Actor");
    auto* pawn = reflect::find_class(L"/Script/Engine.Pawn");
    auto* cp = reflect::find_class(L"/Script/Zenith.ZenithCompanionCharacter");
    auto* ai = reflect::find_class(L"/Script/Zenith.ZenithAICompanionController");
    auto* scene = reflect::find_class(L"/Script/Engine.SceneComponent");
    auto* component = reflect::find_class(L"/Script/Engine.ActorComponent");
    auto* spine = reflect::find_class(L"/Script/SpinePlugin.SpineSkeletonComponent");
    auto* particle = reflect::find_class(L"/Script/Engine.ParticleSystemComponent");
    reflect::Resolver a("companion Actor", actor), p("companion Pawn", pawn), c("Companion", cp), i("CompanionAI", ai);
    reflect::Resolver v("companion Scene", scene), ac("companion component", component), sk("companion Spine", spine);
    s.owner = a.prop(L"Owner", reflect::Kind::Object);
    s.hidden = a.prop(L"bHidden", reflect::Kind::Bool);
    s.hide = a.func(L"SetActorHiddenInGame", 1);
    s.get_rotation = a.func(L"K2_GetActorRotation", 0);
    s.set_rotation = a.func(L"K2_SetActorRotation", 2);
    s.controller = p.prop(L"Controller", reflect::Kind::Object);
    s.get_controller = p.func(L"GetController", 0);
    s.renderer = c.prop(L"SkeletonComponent", reflect::Kind::Object);
    s.skeleton = c.prop(L"SkeletonAnimComponent", reflect::Kind::Object);
    s.is_active = c.func(L"IsActive", 0);
    s.leader = i.func(L"GetLeaderPawn", 0);
    s.scene_hidden = v.prop(L"bHiddenInGame", reflect::Kind::Bool);
    s.visible = v.prop(L"bVisible", reflect::Kind::Bool);
    s.set_hidden = v.func(L"SetHiddenInGame", 2);
    s.set_visible = v.func(L"SetVisibility", 2);
    s.active = ac.prop(L"bIsActive", reflect::Kind::Bool);
    s.activate = ac.func(L"Activate", 1);
    s.deactivate = ac.func(L"Deactivate", 0);
    s.scale_x = sk.func(L"GetScaleX", 0);
    s.set_scale_x = sk.func(L"SetScaleX", 1);
    if (!a.ok() || !p.ok() || !c.ok() || !i.ok() || !v.ok() || !ac.ok() || !sk.ok() || !particle) {
        g_failure = a.failure() + p.failure() + c.failure() + i.failure() + v.failure() + ac.failure() + sk.failure();
        if (!particle) g_failure += "ParticleSystemComponent class unavailable";
        return nullptr;
    }
    s.actor = ObjHandle::of(actor); s.pawn = ObjHandle::of(pawn); s.companion = ObjHandle::of(cp); s.ai = ObjHandle::of(ai);
    s.scene = ObjHandle::of(scene); s.component = ObjHandle::of(component); s.spine = ObjHandle::of(spine);
    s.particle = ObjHandle::of(particle);
    g_failure.clear();
    g_schema = s;
    return &*g_schema;
}

// Blueprint FX is optional. Class handles invalidate the field offset on unload/reload; the live
// property value is read each sample, so replacing/destroying a PSC cannot leave a cached raw pointer.
std::unordered_map<ObjHandle, reflect::PropRef> g_fx_props;
ue::UObject* base_fx(ue::UObject* actor) {
    const auto type = ObjHandle::of(actor->GetClassPrivate());
    auto it = g_fx_props.find(type);
    if (it == g_fx_props.end()) {
        reflect::PropRef prop;
        if (auto* p = reflect::find_property(actor->GetClassPrivate(), L"FX"); p && reflect::kind_of(p) == reflect::Kind::Object) {
            prop.offset = p->GetOffset_Internal();
            prop.kind = reflect::Kind::Object;
        }
        it = g_fx_props.emplace(type, prop).first;
    }
    return it->second.ok() ? it->second.object(actor) : nullptr;
}

std::uint8_t seat_of(ue::UObject* pc) {
    if (!pc) return 0;
    if (pc == world::p1_controller()) return 1;
    for (const auto& seat : party::seats())
        if (seat.slot > 0 && seat.slot <= 255 && core::controller(seat.slot) == pc) return static_cast<std::uint8_t>(seat.slot);
    return 0;
}
bool toggle(const reflect::FuncRef& fn, ue::UObject* object, bool on, bool propagate = false) {
    if (!object) return false;
    reflect::Frame f(fn);
    f.arg_bool(0, on);
    if (fn.count - (fn.ret >= 0 ? 1 : 0) == 2) f.arg_bool(1, propagate);
    return f.invoke(object);
}

struct HostRecord {
    std::uint16_t id = 0;
    std::uint8_t seat = 0;
    ObjHandle owner; // canonical leader player PC, NOT Actor.Owner (PossessedBy changes that to the AIC)
    bool seen = false, published = false;
};
struct Copy {
    ObjHandle actor, controller, managed_fx;
    std::uint16_t cls = 0;
    std::uint8_t seat = 0, fx_bits = 0;
    std::int8_t facing = 0;
    bool seen = false, ready = false;
    bool rotation_set = false;
    std::array<float, 3> rotation{}; // last Host world rotation, reset with the copy's weak identity
};

class Companion final : public IEntityAdapter {
public:
    const char* name() const override { return "companion"; }

    void write_host(HostCtx& c) override {
        for (auto& [h, r] : ids_) r.seen = r.published = false;
        const auto* s = schema();
        if (!s) { ++schema_failed_; return; }
        bool overflow = false;
        for (const auto& e : registry::bucket(registry::Kind::Companion)) {
            auto* a = e.handle.get();
            if (!a) continue;
            if (auto prior = ids_.find(e.handle); prior != ids_.end()) prior->second.seen = true;
            // CF2840's SpawnParameters.Owner is the PC, but APawn::PossessedBy changes Actor.Owner
            // to the AIC. Runtime identity must follow AIC -> LeaderPawn -> player Controller.
            reflect::Frame controller(s->get_controller);
            if (!controller.invoke(a)) { ++sample_failed_; continue; }
            auto* ai = controller.ret<ue::UObject*>();
            if (!ue::alive(ai) || !ai->IsA(static_cast<ue::UClass*>(s->ai.get()))) { ++leader_missing_; continue; }
            reflect::Frame leader(s->leader);
            if (!leader.invoke(ai)) { ++sample_failed_; continue; }
            auto* pawn = leader.ret<ue::UObject*>();
            if (!ue::alive(pawn) || !pawn->IsA(static_cast<ue::UClass*>(s->pawn.get()))) { ++leader_missing_; continue; }
            auto* owner = s->controller.object(pawn);
            const auto seat = seat_of(owner);
            if (!seat) { ++unowned_; continue; }
            // A direct player Owner is a transitional corroboration only. Normal Owner=AIC is not a conflict.
            const auto direct_seat = seat_of(s->owner.object(a));
            if (direct_seat && direct_seat != seat) { ++owner_conflicts_; continue; }
            auto found = ids_.find(e.handle);
            if (found != ids_.end() && (found->second.owner != ObjHandle::of(owner) || found->second.seat != seat)) {
                ids_.erase(found); found = ids_.end(); ++owner_changes_;
            }
            if (found == ids_.end()) {
                if (next_id_ > std::numeric_limits<std::uint16_t>::max()) { ++id_exhausted_; overflow = true; continue; }
                found = ids_.emplace(e.handle, HostRecord{static_cast<std::uint16_t>(next_id_++), seat, ObjHandle::of(owner)}).first;
            }
            auto& rec = found->second;
            rec.seen = true;
            const auto at = core::location(a);
            reflect::Frame rotation(s->get_rotation);
            if (!rotation.invoke(a)) { ++sample_failed_; continue; }
            const auto angles = rotation.ret<std::array<float, 3>>();
            auto* renderer = s->renderer.object(a);
            reflect::Frame active(s->is_active);
            if (!at || !std::isfinite(at->x) || !std::isfinite(at->y) || !std::isfinite(at->z) ||
                !std::isfinite(angles[0]) || !std::isfinite(angles[1]) || !std::isfinite(angles[2]) || !renderer || !active.invoke(a)) {
                ++sample_failed_; continue;
            }
            if (c.frame.cp.n == c.frame.cp.rows.size()) { ++capacity_overflow_; overflow = true; continue; }
            auto* row = c.frame.cp.add(rec.id);
            row->seat = seat;
            row->cls = c.name(log::utf8(a->GetClassPrivate()->GetPathName()));
            row->x = at->x; row->y = at->y; row->z = at->z;
            row->pitch = angles[0]; row->yaw = angles[1]; row->roll = angles[2];
            row->flags = (s->hidden.read_bool(a) ? kHidden : 0) | (active.ret_bool() ? kActive : 0) |
                         (s->scene_hidden.read_bool(renderer) ? kRendererHidden : 0) | (s->visible.read_bool(renderer) ? kRendererVisible : 0);
            if (auto* fx = base_fx(a); fx && fx->IsA(static_cast<ue::UClass*>(s->particle.get()))) {
                row->flags |= kFxExists | (s->active.read_bool(fx) ? kFxActive : 0) |
                              (s->visible.read_bool(fx) ? kFxVisible : 0) | (s->scene_hidden.read_bool(fx) ? kFxHidden : 0);
            }
            if (auto* skeleton = s->skeleton.object(a)) {
                reflect::Frame scale(s->scale_x);
                if (scale.invoke(skeleton)) {
                    const auto value = scale.ret<float>();
                    if (std::isfinite(value)) row->facing = value > 0 ? 1 : value < 0 ? -1 : 0;
                }
            }
            presentation::capture(a, c.name, row->pose);
            visual_state::capture(a, row->visual);
            rec.published = true;
            ++captured_;
        }
        std::erase_if(ids_, [&](const auto& p) { if (p.second.seen && p.first.valid()) return false; ++retired_; return true; });
        if (overflow) {
            // An incomplete baseline must not silently look like a successful partial replication.
            c.frame.cp.clear();
            for (auto& [h, r] : ids_) r.published = false;
            ++closed_frames_;
        }
    }

    void apply_client(const ClientCtx& c) override {
        const auto* s = schema();
        if (!s) {
            ++schema_failed_;
            for (auto& [id, cp] : copies_) {
                cp.ready = false;
                if (auto* a = cp.actor.get()) hide_actor(a);
            }
            return;
        }
        for (auto& [id, cp] : copies_) cp.seen = false;
        for (const auto& row : c.frame.cp.view()) {
            if (!row.key || !row.seat || row.seat == c.frame.me) continue; // local P1's companions retain their native owner.
            auto& cp = copies_[row.key]; cp.seen = true;
            auto* a = cp.actor.get();
            if ((a && (cp.cls != row.cls || cp.seat != row.seat)) || (!a && !cp.controller.empty())) { retire(cp); a = nullptr; }
            cp.seen = true;
            if (!a) {
                const auto* path = c.text(row.cls);
                auto* cls = path ? class_by_path(*path) : nullptr;
                if (!cls || !cls->IsChildOf(static_cast<ue::UClass*>(s->companion.get()))) { ++class_failed_; continue; }
                a = spawn_copy(cls, row.x, row.y, row.z);
                if (!a) { ++spawn_failed_; continue; }
                cp.actor = ObjHandle::of(a); cp.controller = ObjHandle::of(s->controller.object(a));
                cp.cls = row.cls; cp.seat = row.seat; cp.ready = false; cp.facing = 0; cp.managed_fx = {};
                cp.rotation_set = false;
                guard::mark_not_player(a, true);
                ++spawned_;
            }
            const auto controller = ObjHandle::of(s->controller.object(a));
            if (controller != cp.controller) {
                if (auto* old = cp.controller.get(); old && old->IsA(static_cast<ue::UClass*>(s->ai.get()))) {
                    destroy_copy(old); guard::unmark(old);
                }
                cp.controller = controller;
                cp.ready = false;
                ++controller_changes_;
            }
            if (auto* ai = cp.controller.get(); ai && !ai->IsA(static_cast<ue::UClass*>(s->ai.get()))) {
                cp.ready = false; hide_actor(a); ++policy_failed_; continue;
            }
            const bool policy_reapplied = !cp.ready;
            if (!cp.ready) cp.ready = presentation_policy::apply(a);
            if (!cp.ready) { hide_actor(a); ++policy_failed_; continue; }
            bool ok = true;
            if (auto at = core::location(a); !at || std::fabs(at->x - row.x) + std::fabs(at->y - row.y) + std::fabs(at->z - row.z) > 0.25f)
                ok = move_copy(a, row.x, row.y, row.z);
            const std::array<float, 3> angles{row.pitch, row.yaw, row.roll};
            if (!cp.rotation_set || cp.rotation != angles) {
                reflect::Frame rotation(s->set_rotation);
                rotation.arg(0, angles).arg_bool(1, false);
                if (rotation.invoke(a) && rotation.ret_bool()) {
                    cp.rotation = angles; cp.rotation_set = true; ++rotation_writes_;
                } else ok = false;
            }
            if (row.facing && row.facing != cp.facing) {
                auto* skeleton = s->skeleton.object(a);
                if (skeleton) {
                    reflect::Frame value(s->scale_x);
                    if (value.invoke(skeleton) && std::isfinite(value.ret<float>())) {
                        // The wire currently carries a sign, not the complete skeleton scale. Keep the template magnitude.
                        reflect::Frame set(s->set_scale_x);
                        set.arg(0, std::fabs(value.ret<float>()) * (row.facing > 0 ? 1.0f : -1.0f));
                        if (set.invoke(skeleton)) cp.facing = row.facing; else ok = false;
                    } else ok = false;
                } else ok = false;
            }
            ok = visual_state::apply(a, row.visual) && ok;
            ok = presentation::apply(a, row.pose, c.text) && ok;
            auto* renderer = s->renderer.object(a);
            ok = toggle(s->set_hidden, renderer, (row.flags & kRendererHidden) != 0) && ok;
            ok = toggle(s->set_visible, renderer, (row.flags & kRendererVisible) != 0) && ok;
            // ADR0008: only the exact BP.FX component is owned by this adapter. Host event delivery
            // excludes this same PSC; other particles (including PS_Spirit_CanCast) retain their events.
            const auto fx_bits = static_cast<std::uint8_t>(row.flags & 0xF0);
            if (auto* fx = base_fx(a); fx && fx->IsA(static_cast<ue::UClass*>(s->particle.get()))) {
                const auto h = ObjHandle::of(fx);
                const bool exists = (fx_bits & kFxExists) != 0;
                const bool active = exists && (fx_bits & kFxActive);
                const bool visible = exists && (fx_bits & kFxVisible);
                const bool hidden = !exists || (fx_bits & kFxHidden);
                if (policy_reapplied || cp.managed_fx != h || active != s->active.read_bool(fx)) {
                    if (active) ok = toggle(s->activate, fx, false) && ok; // bReset=false; never reset an already active PSC each frame.
                    else ok = reflect::Frame(s->deactivate).invoke(fx) && ok;
                    ++fx_active_writes_;
                }
                if (visible != s->visible.read_bool(fx)) { ok = toggle(s->set_visible, fx, visible) && ok; ++fx_visibility_writes_; }
                if (hidden != s->scene_hidden.read_bool(fx)) { ok = toggle(s->set_hidden, fx, hidden) && ok; ++fx_visibility_writes_; }
                cp.managed_fx = h;
            } else if (fx_bits & kFxExists) { ++fx_missing_; ok = false; }
            if (fx_bits != cp.fx_bits) { ++fx_state_changes_; cp.fx_bits = fx_bits; }
            if ((fx_bits & (kFxExists | kFxActive | kFxVisible | kFxHidden)) == (kFxExists | kFxActive | kFxVisible)) ++host_fx_visible_;
            // IsActive is diagnostic; it does not decide renderer visibility or restart Companion AI.
            if (!(row.flags & kActive)) ++inactive_samples_;
            ok = toggle(s->hide, a, !ok || (row.flags & kHidden)) && ok;
            if (!ok) { ++apply_failed_; hide_actor(a); cp.ready = false; }
            else ++applied_;
        }
        for (auto it = copies_.begin(); it != copies_.end();) {
            if (it->second.seen) { ++it; continue; }
            retire(it->second); it = copies_.erase(it);
        }
    }

    void reset() override {
        for (auto& [id, cp] : copies_) retire(cp);
        copies_.clear(); ids_.clear(); g_fx_props.clear(); g_schema.reset();
        // next_id_ survives map changes; delayed event IDs never address a new object in this session.
    }
    void end_session() { reset(); next_id_ = 1; }
    std::uint16_t id_of(const ObjHandle& h) const {
        const auto it = ids_.find(h);
        return it != ids_.end() && it->second.published ? it->second.id : 0;
    }
    std::uint8_t seat_for(std::uint16_t id) const {
        for (const auto& [h, r] : ids_) if (r.id == id) return r.seat;
        return 0;
    }
    ue::UObject* actor_for(std::uint16_t id) const {
        const auto it = copies_.find(id);
        return it != copies_.end() && it->second.ready ? it->second.actor.get() : nullptr;
    }
    std::string describe() const override {
        return std::format("companion: hostIds={} nextId={} copies={} captured={} applied={} spawned={} removed={} retired={} schemaFail={} sampleFail={} "
                           "ownerConflict={} ownerChange={} leaderMissing={} unowned={} controllerChange={} capacityOverflow={} idExhausted={} closedFrames={} "
                           "classFail={} spawnFail={} policyFail={} applyFail={} inactiveSamples={} fxActiveWrites={} fxVisibilityWrites={} fxMissing={} hostFxVisibleSamples={} "
                           "fxStateChanges={} actorRotationWrites={} fxPhaseColorPending=1 facing=sign-only last='{}'\n",
                           ids_.size(), next_id_, copies_.size(), captured_, applied_, spawned_, removed_, retired_, schema_failed_, sample_failed_,
                           owner_conflicts_, owner_changes_, leader_missing_, unowned_, controller_changes_, capacity_overflow_, id_exhausted_, closed_frames_,
                           class_failed_, spawn_failed_, policy_failed_, apply_failed_, inactive_samples_, fx_active_writes_, fx_visibility_writes_, fx_missing_, host_fx_visible_, fx_state_changes_, rotation_writes_, g_failure);
    }

private:
    void retire(Copy& cp) {
        const auto* s = schema();
        const auto destroy_controller = [&](const ObjHandle& handle) {
            if (auto* controller = handle.get(); controller && s && controller->IsA(static_cast<ue::UClass*>(s->ai.get()))) {
                destroy_copy(controller); guard::unmark(controller);
            }
        };
        if (auto* a = cp.actor.get()) {
            const auto current = s ? ObjHandle::of(s->controller.object(a)) : ObjHandle{};
            // Destruction can fire vanilla events: keep settlement marks until both owned actors are gone.
            presentation_policy::release(a); presentation::forget(a); visual_state::forget(a);
            // Native Companion removal (IDA CD7930) retires the AIC before its Pawn.
            destroy_controller(cp.controller);
            if (current != cp.controller) destroy_controller(current);
            destroy_copy(a);
            guard::mark_not_player(a, false); guard::unmark(a);
        } else destroy_controller(cp.controller);
        cp = {};
        ++removed_;
    }
    // ObjHandle serial + owner changes retire identities. Explicit reset invalidates every class/FX cache.
    std::unordered_map<ObjHandle, HostRecord> ids_;
    std::map<std::uint16_t, Copy> copies_;
    std::uint32_t next_id_ = 1;
    std::uint64_t captured_ = 0, applied_ = 0, spawned_ = 0, removed_ = 0, retired_ = 0;
    std::uint64_t schema_failed_ = 0, sample_failed_ = 0, owner_conflicts_ = 0, owner_changes_ = 0, leader_missing_ = 0, unowned_ = 0, controller_changes_ = 0;
    std::uint64_t capacity_overflow_ = 0, id_exhausted_ = 0, closed_frames_ = 0, class_failed_ = 0, spawn_failed_ = 0, policy_failed_ = 0, apply_failed_ = 0;
    std::uint64_t inactive_samples_ = 0, fx_active_writes_ = 0, fx_visibility_writes_ = 0, fx_missing_ = 0, host_fx_visible_ = 0, fx_state_changes_ = 0;
    std::uint64_t rotation_writes_ = 0;
};

Companion& instance() { static Companion adapter; return adapter; }
} // namespace

IEntityAdapter& companion() { return instance(); }
std::uint16_t companion_id_of(const ObjHandle& actor) { return instance().id_of(actor); }
std::uint8_t companion_seat_of(std::uint16_t id) { return instance().seat_for(id); }
ue::UObject* companion_actor_of(std::uint16_t id) { return instance().actor_for(id); }
bool companion_owns_fx(ue::UObject* owner, ue::UObject* psc) {
    if (!ue::on_game_thread() || !ue::alive(owner) || !ue::alive(psc)) return false;
    const auto* s = schema();
    return s && owner->IsA(static_cast<ue::UClass*>(s->companion.get())) && psc->IsA(static_cast<ue::UClass*>(s->particle.get())) && base_fx(owner) == psc;
}
void companion_end_session() { instance().end_session(); }

} // namespace elmp::coop::entities
