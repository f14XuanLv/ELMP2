#include "coop/fx.h"

#include "guard/guard.h"

#include "engine/handle.h"
#include "engine/reflect.h"

#include "base/hook.h"
#include "base/log.h"

#include <cstring>
#include <format>
#include <unordered_map>

namespace elmp::coop::fx {

namespace {

using RC::Unreal::UObject;

// UBaseFXComponent::SpawnFX_Implementation (IDA 0x7D0600, called by execSpawnFX through vtable +1024):
// (this, PS, ESpineBone, FName CustomBone, const FTransform* Offset, bAttach, bFollowBoneRotation,
//  int32 TranslucencySortPriority, EPSCPoolMethod, bRegisterAsRunningFX) → UParticleSystemComponent*
using SpawnFn = UObject*(__fastcall*)(UObject*, UObject*, std::uint8_t, std::uint64_t, const std::uint8_t*, bool, bool, std::int32_t, std::uint8_t, bool);
using DeactivateFn = void(__fastcall*)(UObject*);
// UFMODBlueprintStatics::PlayEventAtLocation (IDA 0x78D570): (WorldContext, UFMODEvent*, const FTransform&,
// bAutoPlay) → FFMODEventInstance (one pointer, returned in rax). PlayEvent2D (0x78D520) forwards here with an
// identity transform; the sound timeline (0xD24B50), abilities and blueprints all end here.
using SoundFn = std::uint64_t(__fastcall*)(UObject*, UObject*, const std::uint8_t*, bool);
// PlayEventAttached (0x78D7A0): counted only (attached / looping sounds are not replicated yet)
using AttachedFn = UObject*(__fastcall*)(UObject*, UObject*, std::uint64_t, const float*, std::uint8_t, bool, bool, bool);
using StopFn = void(__fastcall*)(UObject*, std::uint8_t);

SpawnFn o_spawn{};
DeactivateFn o_deactivate{};
SoundFn o_sound{};
AttachedFn o_attached{};
StopFn o_stop{};
std::uint64_t g_attached = 0, g_local_sound_blocked = 0;

Sinks g_sinks;
bool g_host = false, g_client = false, g_block = true, g_replaying = false;
bool g_spawn_ok = false, g_deact_ok = false, g_sound_ok = false, g_stop_ok = false, g_tried = false;
std::uint64_t g_cap_spawn = 0, g_cap_deact = 0, g_cap_sound = 0, g_cap_stop = 0;
std::uint64_t g_rep_spawn = 0, g_rep_fail = 0, g_rep_sound = 0, g_rep_stop = 0, g_rep_deact = 0, g_local_blocked = 0;
std::unordered_map<std::string, std::uint64_t> g_blocked_ps; // Dev: which local FX were suppressed (first 32 kinds)

void read_xf(const std::uint8_t* t, std::array<float, 10>& xf) {
    std::memcpy(&xf[0], t, 16);
    std::memcpy(&xf[4], t + 16, 12);
    std::memcpy(&xf[7], t + 32, 12);
}
void write_xf(std::uint8_t* t, const std::array<float, 10>& xf) {
    std::memset(t, 0, 48);
    std::memcpy(t, &xf[0], 16);
    std::memcpy(t + 16, &xf[4], 12);
    std::memcpy(t + 32, &xf[7], 12);
}

void hide_now(UObject* psc) {
    static reflect::FuncRef vis = reflect::Resolver("SceneComponent", reflect::find_class(L"/Script/Engine.SceneComponent")).func(L"SetVisibility", 2);
    if (vis.ok()) {
        reflect::Frame f(vis);
        f.arg_bool(0, false).arg_bool(1, true);
        f.invoke(psc);
    }
    deactivate(psc);
}

UObject* __fastcall d_spawn(UObject* comp, UObject* ps, std::uint8_t bone, std::uint64_t custom, const std::uint8_t* xf, bool attach, bool follow, std::int32_t sort,
                            std::uint8_t pool, bool reg) {
    auto* psc = o_spawn(comp, ps, bone, custom, xf, attach, follow, sort, pool, reg);
    if (!ue::on_game_thread() || !comp) return psc;
    auto* owner = comp->GetOuterPrivate();
    if (g_host && g_sinks.spawn && ps && xf) {
        Spawn s;
        s.owner = owner;
        s.ps = ps;
        s.psc = psc;
        s.msg.bone = bone;
        read_xf(xf, s.msg.xf);
        s.msg.flags = (attach ? 1 : 0) | (follow ? 2 : 0) | (reg ? 4 : 0);
        s.msg.sort = sort;
        s.msg.pool = pool;
        ue::FName n;
        std::memcpy(&n, &custom, sizeof(n));
        s.custom_bone = ue::utf8(n);
        ++g_cap_spawn;
        g_sinks.spawn(s);
    } else if (g_client && g_block && !g_replaying && owner && guard::marked(owner) && ue::alive(psc)) {
        // a presented actor's own FX (anim notify / leftover timeline): the host's copy is replayed instead
        ++g_local_blocked;
        if (ps && g_blocked_ps.size() < 32) ++g_blocked_ps[path_of(ps)];
        hide_now(psc);
    }
    return psc;
}

void __fastcall d_deactivate(UObject* psc) {
    if (g_host && g_sinks.deactivate && psc && ue::on_game_thread()) ++g_cap_deact, g_sinks.deactivate(psc);
    o_deactivate(psc);
}

// The sound's owner: the world context itself when it is an actor, else the actor owning the component.
// Abilities are skipped (the client plays a replayed ability's own sounds).
UObject* sound_owner(UObject* ctx) {
    static ue::UClass* actor = reflect::find_class(L"/Script/Engine.Actor");
    static ue::UClass* comp = reflect::find_class(L"/Script/Engine.ActorComponent");
    static ue::UClass* ability = reflect::find_class(L"/Script/Zenith.Ability");
    if (!ctx || !actor || !comp) return nullptr;
    auto* cls = ctx->GetClassPrivate();
    UObject* a = cls->IsChildOf(actor) ? ctx : cls->IsChildOf(comp) ? ctx->GetOuterPrivate() : nullptr;
    if (!a || (ability && a->GetClassPrivate()->IsChildOf(ability))) return nullptr;
    return a;
}

std::uint64_t __fastcall d_sound(UObject* ctx, UObject* ev, const std::uint8_t* xf, bool autoplay) {
    if (ev && ue::on_game_thread()) {
        if (g_host && g_sinks.sound) {
            if (auto* owner = sound_owner(ctx)) {
                float t[3]{};
                if (xf) std::memcpy(t, xf + 16, 12);
                ++g_cap_sound;
                g_sinks.sound({owner, ev, t[0] == 0 && t[1] == 0 && t[2] == 0}); // PlayEvent2D passes the identity transform
            }
        } else if (g_client && g_block && !g_replaying) {
            if (auto* owner = sound_owner(ctx); owner && guard::marked(owner)) return ++g_local_sound_blocked, 0; // the host's sound arrives instead
        }
    }
    return o_sound(ctx, ev, xf, autoplay);
}

UObject* __fastcall d_attached(UObject* ev, UObject* parent, std::uint64_t socket, const float* at, std::uint8_t rule, bool stop, bool play, bool destroy) {
    if (ue::on_game_thread()) ++g_attached;
    return o_attached(ev, parent, socket, at, rule, stop, play, destroy);
}

void __fastcall d_stop(UObject* ability, std::uint8_t reason) {
    if (g_host && g_sinks.ability_stop && ability && ue::on_game_thread()) ++g_cap_stop, g_sinks.ability_stop(ability, reason);
    o_stop(ability, reason);
}

void install() {
    if (g_tried) return;
    g_tried = true;
    g_spawn_ok = hook::install_manifest("SpawnFXImpl", reinterpret_cast<void*>(&d_spawn), reinterpret_cast<void**>(&o_spawn));
    g_deact_ok = hook::install_manifest("PSCDeactivateSystem", reinterpret_cast<void*>(&d_deactivate), reinterpret_cast<void**>(&o_deactivate));
    g_sound_ok = hook::install_manifest("FMODPlayEventAtLocation", reinterpret_cast<void*>(&d_sound), reinterpret_cast<void**>(&o_sound));
    hook::install_manifest("FMODPlayEventAttached", reinterpret_cast<void*>(&d_attached), reinterpret_cast<void**>(&o_attached));
    g_stop_ok = hook::install_manifest("AbilityStop", reinterpret_cast<void*>(&d_stop), reinterpret_cast<void**>(&o_stop));
}

ue::UObject* component_of(ue::UObject* actor, const wchar_t* cls_path) {
    static reflect::FuncRef by_class = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"GetComponentByClass", 1);
    auto* cls = reflect::find_class(cls_path);
    if (!by_class.ok() || !cls || !actor) return nullptr;
    reflect::Frame f(by_class);
    f.arg(0, static_cast<ue::UObject*>(cls));
    return f.invoke(actor) ? f.ret<ue::UObject*>() : nullptr;
}

ue::UObject* asset(const std::string& path) {
    static std::unordered_map<std::string, ObjHandle> cache; // path → asset (weak; reloaded when unloaded)
    auto& h = cache[path];
    if (auto* o = h.get()) return o;
    auto* o = reflect::load_object(log::wide(path));
    h = ObjHandle::of(o);
    return o;
}

} // namespace

bool host_enable(const Sinks& s) {
    install();
    g_sinks = s;
    g_host = g_spawn_ok || g_sound_ok || g_stop_ok;
    return g_host;
}

void host_disable() { g_host = false, g_sinks = {}; }

bool client_enable() {
    install();
    g_client = g_spawn_ok;
    return g_client;
}

void client_disable() { g_client = false; }
void set_block_local(bool on) { g_block = on; }

std::string path_of(ue::UObject* a) {
    static std::unordered_map<ue::UObject*, std::pair<ObjHandle, std::string>> paths;
    auto& e = paths[a];
    if (e.first.get() != a) e = {ObjHandle::of(a), log::utf8(a->GetPathName())};
    return e.second;
}

ue::UObject* replay_spawn(ue::UObject* actor, const net::proto::Fx& m, const std::string& ps_path, const std::string& custom_bone, std::string* why) {
    auto fail = [&](const char* w) -> ue::UObject* {
        ++g_rep_fail;
        if (why) *why = w;
        return nullptr;
    };
    static ue::UClass* ps_cls = reflect::find_class(L"/Script/Engine.ParticleSystem");
    auto* ps = asset(ps_path);
    if (!ps || !ps_cls || !ps->GetClassPrivate()->IsChildOf(ps_cls)) return fail("not a ParticleSystem");
    auto* comp = component_of(actor, L"/Script/SpineExtensionPlugin.BaseFXComponent");
    if (!ue::alive(comp)) return fail("no FX component");
    static reflect::FuncRef spawn = reflect::Resolver("BaseFXComponent", reflect::find_class(L"/Script/SpineExtensionPlugin.BaseFXComponent")).func(L"SpawnFX", 9);
    if (!spawn.ok()) return fail("SpawnFX");
    reflect::Frame f(spawn);
    f.arg(0, ps).arg(1, m.bone).arg_name(2, custom_bone.empty() ? ue::FName() : ue::FName(log::wide(custom_bone).c_str(), RC::Unreal::FNAME_Add));
    if (auto* t = static_cast<std::uint8_t*>(f.raw(3))) write_xf(t, m.xf);
    f.arg_bool(4, m.flags & 1).arg_bool(5, m.flags & 2).arg(6, m.sort).arg(7, m.pool).arg_bool(8, m.flags & 4);
    g_replaying = true;
    const bool ok = f.invoke(comp);
    g_replaying = false;
    if (!ok) return fail("invoke");
    ++g_rep_spawn;
    return f.ret<ue::UObject*>();
}

bool deactivate(ue::UObject* psc) {
    static reflect::FuncRef d = reflect::Resolver("ActorComponent", reflect::find_class(L"/Script/Engine.ActorComponent")).func(L"Deactivate", 0);
    if (!ue::alive(psc) || !d.ok()) return false;
    if (g_client) ++g_rep_deact;
    return reflect::Frame(d).invoke(psc);
}

bool play_sound(ue::UObject* actor, const std::string& event_path, bool is2d, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    static ue::UClass* ev_cls = reflect::find_class(L"/Script/FMODStudio.FMODEvent");
    auto* ev = asset(event_path);
    if (!ev || !ev_cls || !ev->GetClassPrivate()->IsChildOf(ev_cls)) return fail("not an FMODEvent");
    static reflect::Resolver r("FMODBlueprintStatics", reflect::find_class(L"/Script/FMODStudio.FMODBlueprintStatics"));
    static reflect::FuncRef at = r.func(L"PlayEventAtLocation", 4), flat = r.func(L"PlayEvent2D", 3);
    static reflect::FuncRef xf = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"GetTransform", 0);
    auto* cdo = reflect::find_object(L"/Script/FMODStudio.Default__FMODBlueprintStatics");
    if (!cdo || !at.ok() || !flat.ok() || !xf.ok()) return fail("FMOD statics");
    bool ok = false;
    g_replaying = true;
    if (is2d) {
        reflect::Frame f(flat);
        f.arg(0, actor).arg(1, ev).arg_bool(2, true);
        ok = f.invoke(cdo);
    } else if (reflect::Frame t(xf); t.invoke(actor)) {
        reflect::Frame f(at);
        f.arg(0, actor).arg(1, ev);
        if (void* dst = f.raw(2)) std::memcpy(dst, t.raw(xf.ret), 48);
        f.arg_bool(3, true);
        ok = f.invoke(cdo);
    }
    g_replaying = false;
    if (!ok) return fail("invoke");
    ++g_rep_sound;
    return true;
}

bool stop_ability(ue::UObject* ability, std::uint8_t reason) {
    static reflect::FuncRef stop = reflect::Resolver("Ability", reflect::find_class(L"/Script/Zenith.Ability")).func(L"Stop", 1);
    if (!ue::alive(ability) || !stop.ok()) return false;
    static reflect::FuncRef launched = reflect::Resolver("Ability", reflect::find_class(L"/Script/Zenith.Ability")).func(L"IsLaunched", 0);
    if (launched.ok()) {
        reflect::Frame l(launched);
        if (!l.invoke(ability) || !l.ret_bool()) return false; // already ended locally (fire-and-forget hit a wall)
    }
    reflect::Frame f(stop);
    f.arg(0, reason);
    if (!f.invoke(ability)) return false;
    ++g_rep_stop;
    return true;
}

std::string describe() {
    std::string out = std::format("fx: hooks spawn={} deact={} sound={} stop={} | host captured spawn={} deact={} sound={} stop={} | client replayed spawn={} fail={} deact={} sound={} stop={} localBlocked fx={} sound={} | attachedSounds={}{}\n",
                                  g_spawn_ok, g_deact_ok, g_sound_ok, g_stop_ok, g_cap_spawn, g_cap_deact, g_cap_sound, g_cap_stop, g_rep_spawn, g_rep_fail, g_rep_deact, g_rep_sound,
                                  g_rep_stop, g_local_blocked, g_local_sound_blocked, g_attached, g_block ? "" : " (blocking off)");
    for (const auto& [p, n] : g_blocked_ps) out += std::format("  local {} x{}\n", p.substr(p.rfind('/') + 1), n);
    return out;
}

} // namespace elmp::coop::fx
