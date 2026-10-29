#include "guard/guard.h"

#include "base/hook.h"
#include "base/log.h"
#include "base/metrics.h"

#include <Unreal/UObject.hpp>

#include <format>
#include <unordered_set>

namespace elmp::guard {

namespace {

using RC::Unreal::UObject;

bool g_installed = false, g_install_failed = false, g_armed = false;
std::unordered_set<UObject*> g_marked, g_not_player, g_replay, g_tick_allow;
Counters g_c{};

using DoDamageFn = void(__fastcall*)(UObject*, UObject*, int, bool, bool);
using SkillFn = void(__fastcall*)(void*, UObject*, UObject*, void*);
using IsPlayerFn = bool(__fastcall*)(UObject*);
using SaveFn = bool(__fastcall*)(UObject*);
using SaveAsyncFn = void(__fastcall*)(UObject*, void*, void*);
using VoidFn = void(__fastcall*)(UObject*);
using BoolSetFn = void(__fastcall*)(UObject*, bool);

DoDamageFn o_damage{};
SkillFn o_skill{};
IsPlayerFn o_isplayer{};
SaveFn o_save{};
SaveAsyncFn o_save_async{};
VoidFn o_ai_activate{}, o_char_activate{};
BoolSetFn o_collision{}, o_tick{};

UObject* owner_of(UObject* component) { return component ? component->GetOuterPrivate() : nullptr; }

// Controller → pawn: AController::Pawn is reflected; read it via the generic property lookup once.
UObject* controlled_pawn(UObject* controller) {
    static std::int32_t offset = [] {
        auto* cls = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr, STR("/Script/Engine.Controller"));
        auto* p = cls ? cls->FindProperty(RC::Unreal::FName(STR("Pawn"))) : nullptr;
        return p ? p->GetOffset_Internal() : -1;
    }();
    return offset < 0 ? nullptr : *reinterpret_cast<UObject**>(reinterpret_cast<std::uint8_t*>(controller) + offset);
}

void __fastcall d_damage(UObject* hp, UObject* from, int dmg, bool just, bool guarding) {
    if (g_armed) {
        auto* target = owner_of(hp);
        if (!g_replay.count(target)) return void(++g_c.damage_blocked);
        ++g_c.damage_replayed;
    }
    o_damage(hp, from, dmg, just, guarding);
}

void __fastcall d_skill(void* self, UObject* instigator, UObject* target, void* entry) {
    if (g_armed) {
        if (!g_replay.count(target)) return void(++g_c.skill_blocked);
        ++g_c.skill_replayed;
    }
    o_skill(self, instigator, target, entry);
}

bool __fastcall d_isplayer(UObject* actor) {
    if (g_armed && actor && (g_not_player.count(actor) || g_marked.count(actor))) {
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
    if (g_armed) return void(++g_c.ai_activate_blocked);
    o_ai_activate(controller);
}

void __fastcall d_char_activate(UObject* pawn) {
    if (g_armed && g_marked.count(pawn)) return void(++g_c.char_activate_blocked);
    o_char_activate(pawn);
}

void __fastcall d_collision(UObject* actor, bool on) {
    if (g_armed && on && g_marked.count(actor)) return void(++g_c.collision_blocked);
    o_collision(actor, on);
}

void __fastcall d_tick(UObject* actor, bool on) {
    if (g_armed && on && g_marked.count(actor) && !g_tick_allow.count(actor)) return void(++g_c.tick_blocked);
    o_tick(actor, on);
}

bool install() {
    if (g_installed) return true;
    if (g_install_failed) return false;
    bool ok = true;
    ok &= hook::install_manifest("GuardDoDamage", reinterpret_cast<void*>(&d_damage), reinterpret_cast<void**>(&o_damage));
    ok &= hook::install_manifest("GuardSkillDamage", reinterpret_cast<void*>(&d_skill), reinterpret_cast<void**>(&o_skill));
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

void mark(ue::UObject* a) {
    if (a) g_marked.insert(a);
}
void unmark(ue::UObject* a) { g_marked.erase(a); }
void unmark_all() {
    g_marked.clear();
    g_not_player.clear();
    g_replay.clear();
    g_tick_allow.clear();
}
bool marked(ue::UObject* a) { return g_marked.count(a) != 0; }
void allow_tick(ue::UObject* a, bool on) {
    if (!a) return;
    if (on) g_tick_allow.insert(a);
    else g_tick_allow.erase(a);
}
void mark_not_player(ue::UObject* a, bool on) {
    if (!a) return;
    if (on) g_not_player.insert(a);
    else g_not_player.erase(a);
}
void open_replay(ue::UObject* t) {
    if (t) g_replay.insert(t);
}
void close_replay(ue::UObject* t) { g_replay.erase(t); }
Counters counters() { return g_c; }

std::string describe() {
    return std::format("guard: installed={} armed={} marked={} notPlayer={} replay={} | damage blocked={} replayed={} skill blocked={} replayed={} isPlayerFalse={} saveBlocked={} "
                       "aiActivateBlocked={} charActivateBlocked={} collisionBlocked={} tickBlocked={}\n",
                       g_installed, g_armed, g_marked.size(), g_not_player.size(), g_replay.size(), g_c.damage_blocked, g_c.damage_replayed, g_c.skill_blocked, g_c.skill_replayed,
                       g_c.isplayer_false, g_c.save_blocked, g_c.ai_activate_blocked, g_c.char_activate_blocked, g_c.collision_blocked, g_c.tick_blocked);
}

} // namespace elmp::guard
