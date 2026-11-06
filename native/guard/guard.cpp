#include "guard/guard.h"

#include "base/hook.h"
#include "base/log.h"
#include "base/metrics.h"
#include "engine/handle.h"

#include <Unreal/UObject.hpp>

#include <format>
#include <unordered_set>

namespace elmp::guard {

namespace {

using RC::Unreal::UObject;

bool g_installed = false, g_install_failed = false, g_armed = false;
bool g_effects_installed = false;
EffectObserver g_effect_observer = nullptr;
// Identity includes the UObject serial: an allocation reusing an address must never inherit a
// presentation role or damage permission. Release/disarm clears explicitly; describe prunes stale
// handles as a low-frequency audit, without adding a world scan to any detour.
std::unordered_set<ObjHandle> g_marked, g_not_player, g_replay;
std::uint64_t g_stale_pruned = 0;
Counters g_c{};

using DoDamageFn = void(__fastcall*)(UObject*, UObject*, int, bool, bool);
using SkillFn = void(__fastcall*)(void*, UObject*, UObject*, void*);
// [E-IDA][E-SDK] enemy-sync-graph-v2 §4.1: the thunks receive a pointer to 288-byte
// FHitInfos, NOT the enclosing 336-byte FHitEntry used by the ability queue.
using EffectFn = void(__fastcall*)(UObject*, UObject*, UObject*, const void*);
using IsPlayerFn = bool(__fastcall*)(UObject*);
using SaveFn = bool(__fastcall*)(UObject*);
using SaveAsyncFn = void(__fastcall*)(UObject*, void*, void*);
using VoidFn = void(__fastcall*)(UObject*);
using BoolSetFn = void(__fastcall*)(UObject*, bool);

DoDamageFn o_damage{};
SkillFn o_skill{};
EffectFn o_effect_apply{}, o_effect_finalize{};
IsPlayerFn o_isplayer{};
SaveFn o_save{};
SaveAsyncFn o_save_async{};
VoidFn o_ai_activate{}, o_char_activate{}, o_death_start{};
BoolSetFn o_collision{}, o_tick{};

UObject* owner_of(UObject* component) { return component ? component->GetOuterPrivate() : nullptr; }

void __fastcall d_damage(UObject* hp, UObject* from, int dmg, bool just, bool guarding) {
    if (g_armed) {
        auto* target = owner_of(hp);
        if (!g_replay.contains(ObjHandle::of(target))) return void(++g_c.damage_blocked);
        ++g_c.damage_replayed;
    }
    o_damage(hp, from, dmg, just, guarding);
}

void __fastcall d_skill(void* self, UObject* instigator, UObject* target, void* entry) {
    if (g_armed) {
        if (!g_replay.contains(ObjHandle::of(target))) return void(++g_c.skill_blocked);
        ++g_c.skill_replayed;
    }
    o_skill(self, instigator, target, entry);
}

void __fastcall d_effect_apply(UObject* self, UObject* from, UObject* to, const void* hit_infos) {
    // Host presentation does not grant the client's effect graph settlement authority. Apply and
    // Finalize must use the same policy; the latter can run even when Apply was denied.
    if (g_armed) return void(++g_c.effect_apply_blocked);
    struct Scope {
        EffectObserver observer = g_effect_observer;
        UObject *self, *from, *to;
        Scope(UObject* s, UObject* f, UObject* t) : self(s), from(f), to(t) { if (observer) observer(true, self, from, to, false); }
        ~Scope() { if (observer) observer(false, self, from, to, false); }
    } scope(self, from, to);
    o_effect_apply(self, from, to, hit_infos);
}

void __fastcall d_effect_finalize(UObject* self, UObject* from, UObject* to, const void* hit_infos) {
    if (g_armed) return void(++g_c.effect_finalize_blocked);
    struct Scope {
        EffectObserver observer = g_effect_observer;
        UObject *self, *from, *to;
        Scope(UObject* s, UObject* f, UObject* t) : self(s), from(f), to(t) { if (observer) observer(true, self, from, to, true); }
        ~Scope() { if (observer) observer(false, self, from, to, true); }
    } scope(self, from, to);
    o_effect_finalize(self, from, to, hit_infos);
}

void __fastcall d_death_start(UObject* self) {
    // Damage replay is deliberately narrower: it cannot authorize drops, spawn-point notification,
    // save/progress side effects, or the vanilla death graph. Pure death presentation is a later domain.
    if (g_armed) return void(++g_c.death_start_blocked);
    o_death_start(self);
}

bool __fastcall d_isplayer(UObject* actor) {
    if (g_armed && actor && (g_not_player.contains(ObjHandle::of(actor)) || marked(actor))) {
        ++g_c.isplayer_false;
        return false;
    }
    return o_isplayer(actor);
}

bool __fastcall d_save(UObject* self) {
    if (g_armed) return ++g_c.save_blocked, false;
    return o_save(self);
}

void __fastcall d_save_async(UObject* self, void* widget, void* result) {
    if (g_armed) return void(++g_c.save_blocked);
    o_save_async(self, widget, result);
}

void __fastcall d_ai_activate(UObject* controller) {
    // Armed = guest process: no local AI at all (every enemy is host-owned; activation happens at spawn,
    // before takeover could mark it — engine-facts「敌人激活入口重新定位」).
    if (g_armed) {
        // Native equipment owns Companion AIs; their leader-following graph is part of presentation.
        // Enemy AI subclasses retain the unconditional authority gate.
        static ObjHandle companion;
        if (!companion.valid()) companion = ObjHandle::of(RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, STR("/Script/Zenith.ZenithAICompanionController")));
        auto* cls = static_cast<RC::Unreal::UClass*>(companion.get());
        if (!controller || !cls || !controller->IsA(cls)) return void(++g_c.ai_activate_blocked);
    }
    o_ai_activate(controller);
}

void __fastcall d_char_activate(UObject* pawn) {
    if (g_armed && marked(pawn)) return void(++g_c.char_activate_blocked);
    o_char_activate(pawn);
}

void __fastcall d_collision(UObject* actor, bool on) {
    if (g_armed && on && marked(actor)) return void(++g_c.collision_blocked);
    o_collision(actor, on);
}

void __fastcall d_tick(UObject* actor, bool on) {
    if (g_armed && on && marked(actor)) return void(++g_c.tick_blocked);
    o_tick(actor, on);
}

bool install_effects() {
    if (g_effects_installed) return true;
    const bool apply = hook::install_manifest("GuardEffectApply", reinterpret_cast<void*>(&d_effect_apply), reinterpret_cast<void**>(&o_effect_apply));
    const bool finalize = hook::install_manifest("GuardEffectFinalize", reinterpret_cast<void*>(&d_effect_finalize), reinterpret_cast<void**>(&o_effect_finalize));
    return g_effects_installed = apply && finalize;
}

bool install() {
    if (g_installed) return true;
    if (g_install_failed) return false;
    bool ok = true;
    ok &= hook::install_manifest("GuardDoDamage", reinterpret_cast<void*>(&d_damage), reinterpret_cast<void**>(&o_damage));
    ok &= hook::install_manifest("GuardSkillDamage", reinterpret_cast<void*>(&d_skill), reinterpret_cast<void**>(&o_skill));
    ok &= install_effects();
    ok &= hook::install_manifest("GuardDeathStart", reinterpret_cast<void*>(&d_death_start), reinterpret_cast<void**>(&o_death_start));
    ok &= hook::install_manifest("GuardIsPlayerThunk", reinterpret_cast<void*>(&d_isplayer), reinterpret_cast<void**>(&o_isplayer));
    ok &= hook::install_manifest("GuardSaveGame", reinterpret_cast<void*>(&d_save), reinterpret_cast<void**>(&o_save));
    ok &= hook::install_manifest("GuardSaveGameAsync", reinterpret_cast<void*>(&d_save_async), reinterpret_cast<void**>(&o_save_async));
    ok &= hook::install_manifest("GuardEnemyAIActivate", reinterpret_cast<void*>(&d_ai_activate), reinterpret_cast<void**>(&o_ai_activate));
    ok &= hook::install_manifest("GuardEnemyCharacterActivate", reinterpret_cast<void*>(&d_char_activate), reinterpret_cast<void**>(&o_char_activate));
    ok &= hook::install_manifest("GuardActorSetCollision", reinterpret_cast<void*>(&d_collision), reinterpret_cast<void**>(&o_collision));
    ok &= hook::install_manifest("GuardActorSetTick", reinterpret_cast<void*>(&d_tick), reinterpret_cast<void**>(&o_tick));
    if (!ok) {
        g_install_failed = true;
        log::error("guard: a detour could not be installed; client presentation fails closed");
        return false;
    }
    g_installed = true;
    return true;
}

} // namespace

bool arm() {
    if (!install()) return false;
    g_armed = true;
    return true;
}
void disarm() {
    g_armed = false;
    unmark_all();
}
bool armed() { return g_armed; }
bool observe_effects(EffectObserver observer) {
    if (observer && g_effect_observer && g_effect_observer != observer) return false;
    if (observer && !install_effects()) return false;
    g_effect_observer = observer;
    return true;
}

void mark(ue::UObject* a) {
    if (ue::alive(a)) g_marked.insert(ObjHandle::of(a));
}
void unmark(ue::UObject* a) { g_marked.erase(ObjHandle::of(a)); }
void unmark_all() {
    g_marked.clear();
    g_not_player.clear();
    g_replay.clear();
}
bool marked(ue::UObject* a) { return a && g_marked.contains(ObjHandle::of(a)); }
void mark_not_player(ue::UObject* a, bool on) {
    if (!a) return;
    if (on) g_not_player.insert(ObjHandle::of(a));
    else g_not_player.erase(ObjHandle::of(a));
}
void open_replay(ue::UObject* t) {
    if (ue::alive(t)) g_replay.insert(ObjHandle::of(t));
}
void close_replay(ue::UObject* t) { g_replay.erase(ObjHandle::of(t)); }
Counters counters() { return g_c; }

std::string describe() {
    auto expired = [](const ObjHandle& h) { return !h.get(); };
    g_stale_pruned += std::erase_if(g_marked, expired) + std::erase_if(g_not_player, expired) + std::erase_if(g_replay, expired);
    return std::format("guard: installed={} armed={} marked={} notPlayer={} replay={} effectsInstalled={} effectObserver={} | damage blocked={} replayed={} skill blocked={} replayed={} isPlayerFalse={} saveBlocked={} "
                       "aiActivateBlocked={} charActivateBlocked={} collisionBlocked={} tickBlocked={} effectApplyBlocked={} effectFinalizeBlocked={} deathStartBlocked={} stalePruned={}\n",
                       g_installed, g_armed, g_marked.size(), g_not_player.size(), g_replay.size(), g_effects_installed, g_effect_observer != nullptr, g_c.damage_blocked, g_c.damage_replayed, g_c.skill_blocked, g_c.skill_replayed,
                       g_c.isplayer_false, g_c.save_blocked, g_c.ai_activate_blocked, g_c.char_activate_blocked, g_c.collision_blocked, g_c.tick_blocked,
                       g_c.effect_apply_blocked, g_c.effect_finalize_blocked, g_c.death_start_blocked, g_stale_pruned);
}

} // namespace elmp::guard
