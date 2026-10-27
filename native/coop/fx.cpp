#include "coop/fx.h"

#include "core/player.h"
#include "guard/guard.h"

#include "engine/handle.h"
#include "engine/reflect.h"

#include "base/hook.h"
#include "base/log.h"

#include <cmath>
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
                g_sinks.sound({owner, ev, static_cast<std::uint8_t>(t[0] == 0 && t[1] == 0 && t[2] == 0 ? kSound2D : kSoundAt)}); // PlayEvent2D passes the identity transform
            }
        } else if (g_client && g_block && !g_replaying) {
            if (auto* owner = sound_owner(ctx); owner && guard::marked(owner)) return ++g_local_sound_blocked, 0; // the host's sound arrives instead
        }
    }
    return o_sound(ctx, ev, xf, autoplay);
}

std::unordered_map<std::string, std::uint64_t> g_attached_kinds; // Dev: "event @ owner class [marked]" (first 48)

UObject* __fastcall d_attached(UObject* ev, UObject* parent, std::uint64_t socket, const float* at, std::uint8_t rule, bool stop, bool play, bool destroy) {
    // Callers (CUE --imports, 2026-10-28): SpineAnimNotify_PlayFMODEvent (attached anim-notify sounds),
    // CAS_Dash (spirit dash), BP_Character_Player_Base (AudioEventSubmerged), BP_Elevator (MoveSoundEvent).
    // Host: sounds of synced owners are sent like located ones (mode attached); client: a presented owner's own
    // attached sound is suppressed, the host's arrives instead.
    if (ue::on_game_thread() && ev) {
        ++g_attached;
        auto* owner = parent ? sound_owner(parent) : nullptr;
        if (g_attached_kinds.size() < 48)
            g_attached_kinds[std::format("{} @ {}{}", path_of(ev), owner ? log::utf8(ue::class_name_of(owner)) : "-", owner && guard::marked(owner) ? " [marked]" : "")]++;
        if (g_host && g_sinks.sound && owner && play) {
            ++g_cap_sound;
            g_sinks.sound({owner, ev, kSoundAttached});
        } else if (g_client && g_block && !g_replaying && owner && guard::marked(owner)) {
            return ++g_local_sound_blocked, nullptr;
        }
    }
    return o_attached(ev, parent, socket, at, rule, stop, play, destroy);
}

// ---- Blueprint particles outside SpawnFX: UGameplayStatics::SpawnEmitterAttached (0x25463A0) /
// SpawnEmitterAtLocation (0x2546230) [E-IDA]. Callers [E-CUE --imports]: Lily command actions (CA_Guard,
// CA_UseHeal), summon command actions (CAS_Dash, CAS_PileAttack, CACSummonEntry_Charge_Base, CA_s5060_Hook),
// BP_SpiritCharacterBase, BP_CompanionFX_Base, anim notifies (PlayStep, SimulateLanding), world objects
// (BP_Door, BP_Breakable_Base, BP_Interactable_Treasure, BP_SwitchHitbox, BP_HookPoint), ability hitboxes.
// Sent through the FX channel with flags bit3 (emitter): xf = owner-relative location, rotation (quat),
// scale; `sort` carries EAttachLocation; custom bone = AttachPointName. Ability-owned ones are skipped (the
// client's replayed ability spawns its own).
using EmAttachedFn = UObject*(__fastcall*)(UObject*, UObject*, std::uint64_t, const float*, const float*, const float*, std::uint8_t, bool, std::uint8_t, bool);
using EmAtFn = UObject*(__fastcall*)(UObject*, UObject*, const float*, const float*, const float*, bool, std::uint8_t, bool);
EmAttachedFn o_em_attached{};
EmAtFn o_em_at{};
bool g_em_ok = false;
std::uint64_t g_cap_emitter = 0, g_local_emitter_blocked = 0;
std::unordered_map<std::string, std::uint64_t> g_em_kinds; // Dev: captured emitters per particle @ owner class

// the actor a world context / component belongs to (outer chain), nullptr for abilities and non-actors
UObject* actor_owner(UObject* o) {
    static ue::UClass* actor = reflect::find_class(L"/Script/Engine.Actor");
    static ue::UClass* ability = reflect::find_class(L"/Script/Zenith.Ability");
    for (int i = 0; o && actor && i < 8; ++i, o = o->GetOuterPrivate())
        if (o->GetClassPrivate()->IsChildOf(actor)) return ability && o->GetClassPrivate()->IsChildOf(ability) ? nullptr : o;
    return nullptr;
}

constexpr float kDeg = 3.14159265358979f / 180.0f;
void rot_to_quat(const float* r, float* q) { // FRotator (pitch, yaw, roll) → FQuat (UE4.26 FRotator::Quaternion)
    const float sp = std::sin(r[0] * kDeg / 2), cp = std::cos(r[0] * kDeg / 2), sy = std::sin(r[1] * kDeg / 2), cy = std::cos(r[1] * kDeg / 2),
                sr = std::sin(r[2] * kDeg / 2), cr = std::cos(r[2] * kDeg / 2);
    q[0] = cr * sp * sy - sr * cp * cy;
    q[1] = -cr * sp * cy - sr * cp * sy;
    q[2] = cr * cp * sy - sr * sp * cy;
    q[3] = cr * cp * cy + sr * sp * sy;
}
void quat_to_rot(const float* q, float* r) { // FQuat::Rotator
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float test = z * x - w * y;
    const float yaw = std::atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) / kDeg;
    if (test < -0.4999995f) r[0] = -90, r[1] = yaw, r[2] = -yaw - 2 * std::atan2(x, w) / kDeg;
    else if (test > 0.4999995f) r[0] = 90, r[1] = yaw, r[2] = yaw - 2 * std::atan2(x, w) / kDeg;
    else r[0] = std::asin(2 * test) / kDeg, r[1] = yaw, r[2] = std::atan2(-2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) / kDeg;
}

void emitter_captured(UObject* owner, UObject* ps, UObject* psc, std::uint64_t point, const float* loc, const float* rot, const float* scale, std::uint8_t loc_type,
                      std::uint8_t pool, bool attached) {
    if (!ue::on_game_thread() || !owner || !ps) return;
    if (g_host && g_sinks.spawn) {
        Spawn s;
        s.owner = owner;
        s.ps = ps;
        s.psc = psc;
        float rel[3]{loc ? loc[0] : 0, loc ? loc[1] : 0, loc ? loc[2] : 0};
        if (!attached || loc_type == 1) // world position → relative to the owner (map slots differ per process)
            if (const auto at = core::location(owner)) rel[0] -= at->x, rel[1] -= at->y, rel[2] -= at->z;
        if (rot) rot_to_quat(rot, &s.msg.xf[0]);
        std::memcpy(&s.msg.xf[4], rel, 12);
        if (scale) std::memcpy(&s.msg.xf[7], scale, 12);
        s.msg.flags = 8 | (attached ? 1 : 0);
        s.msg.sort = loc_type;
        s.msg.pool = pool;
        ue::FName n;
        std::memcpy(&n, &point, sizeof(n));
        s.custom_bone = attached ? ue::utf8(n) : std::string("None");
        ++g_cap_emitter;
        if (g_em_kinds.size() < 48) ++g_em_kinds[path_of(ps) + " @ " + log::utf8(ue::class_name_of(owner))];
        g_sinks.spawn(s);
    } else if (g_client && g_block && !g_replaying && guard::marked(owner) && ue::alive(psc)) {
        ++g_local_emitter_blocked;
        if (g_blocked_ps.size() < 32) ++g_blocked_ps[path_of(ps)];
        hide_now(psc);
    }
}

UObject* __fastcall d_em_attached(UObject* ps, UObject* comp, std::uint64_t point, const float* loc, const float* rot, const float* scale, std::uint8_t loc_type, bool destroy,
                                  std::uint8_t pool, bool activate) {
    auto* psc = o_em_attached(ps, comp, point, loc, rot, scale, loc_type, destroy, pool, activate);
    if (comp) emitter_captured(actor_owner(comp), ps, psc, point, loc, rot, scale, loc_type, pool, true);
    return psc;
}

UObject* __fastcall d_em_at(UObject* ctx, UObject* ps, const float* loc, const float* rot, const float* scale, bool destroy, std::uint8_t pool, bool activate) {
    auto* psc = o_em_at(ctx, ps, loc, rot, scale, destroy, pool, activate);
    emitter_captured(actor_owner(ctx), ps, psc, 0, loc, rot, scale, 0, pool, false);
    return psc;
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
    g_em_ok = hook::install_manifest("SpawnEmitterAttached", reinterpret_cast<void*>(&d_em_attached), reinterpret_cast<void**>(&o_em_attached)) &
              hook::install_manifest("SpawnEmitterAtLocation", reinterpret_cast<void*>(&d_em_at), reinterpret_cast<void**>(&o_em_at));
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
    if (m.flags & 8) { // blueprint emitter (SpawnEmitterAttached / AtLocation on the host)
        static reflect::Resolver gs("GameplayStatics", reflect::find_class(L"/Script/Engine.GameplayStatics"));
        static reflect::FuncRef em_attached = gs.func(L"SpawnEmitterAttached", 10), em_at = gs.func(L"SpawnEmitterAtLocation", 8);
        static reflect::FuncRef root = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"K2_GetRootComponent", 0);
        auto* cdo = reflect::find_object(L"/Script/Engine.Default__GameplayStatics");
        if (!cdo || !em_attached.ok() || !em_at.ok() || !root.ok()) return fail("GameplayStatics emitters");
        float rot[3];
        quat_to_rot(&m.xf[0], rot);
        float loc[3]{m.xf[4], m.xf[5], m.xf[6]};
        const bool world_pos = !(m.flags & 1) || m.sort == 1;
        if (world_pos) {
            const auto at = core::location(actor);
            if (!at) return fail("owner has no location");
            loc[0] += at->x, loc[1] += at->y, loc[2] += at->z;
        }
        // a fresh component per spawn costs ~0.7 ms (measured: fx_emitter_us p50 701 vs SpawnFX 72); the world PSC pool
        // (EPSCPoolMethod::AutoRelease) reuses finished ones — same lifetime as bAutoDestroy for a fire-and-forget emitter
        const std::uint8_t pool = m.pool ? m.pool : std::uint8_t{1};
        ue::UObject* psc = nullptr;
        g_replaying = true;
        if (m.flags & 1) {
            reflect::Frame rc(root);
            auto* rootc = rc.invoke(actor) ? rc.ret<ue::UObject*>() : nullptr;
            if (ue::alive(rootc)) {
                reflect::Frame f(em_attached);
                f.arg(0, ps).arg(1, rootc).arg_name(2, custom_bone.empty() || custom_bone == "None" ? ue::FName() : ue::FName(log::wide(custom_bone).c_str(), RC::Unreal::FNAME_Add));
                if (void* d = f.raw(3)) std::memcpy(d, loc, 12);
                if (void* d = f.raw(4)) std::memcpy(d, rot, 12);
                if (void* d = f.raw(5)) std::memcpy(d, &m.xf[7], 12);
                f.arg(6, static_cast<std::uint8_t>(m.sort)).arg_bool(7, true).arg(8, pool).arg_bool(9, true);
                if (f.invoke(cdo)) psc = f.ret<ue::UObject*>();
            }
        } else {
            reflect::Frame f(em_at);
            f.arg(0, actor).arg(1, ps);
            if (void* d = f.raw(2)) std::memcpy(d, loc, 12);
            if (void* d = f.raw(3)) std::memcpy(d, rot, 12);
            if (void* d = f.raw(4)) std::memcpy(d, &m.xf[7], 12);
            f.arg_bool(5, true).arg(6, pool).arg_bool(7, true);
            if (f.invoke(cdo)) psc = f.ret<ue::UObject*>();
        }
        g_replaying = false;
        if (!ue::alive(psc)) return fail("emitter invoke");
        ++g_rep_spawn;
        return psc;
    }
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

bool play_sound(ue::UObject* actor, const std::string& event_path, std::uint8_t mode, std::string* why) {
    const bool is2d = mode == kSound2D;
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
    if (mode == kSoundAttached) {
        // PlayEventAttached(Event, AttachToComponent, AttachPointName, Location, LocationType, bStopWhenAttachedToDestroyed, bAutoPlay, bAutoDestroy)
        static reflect::FuncRef attached = r.func(L"PlayEventAttached", 8);
        static reflect::FuncRef root = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"K2_GetRootComponent", 0);
        reflect::Frame rc(root);
        auto* comp = attached.ok() && root.ok() && rc.invoke(actor) ? rc.ret<ue::UObject*>() : nullptr;
        if (!attached.ok()) {
            g_replaying = false;
            return fail("PlayEventAttached");
        }
        if (!ue::alive(comp)) {
            g_replaying = false;
            return fail("owner has no root component");
        }
        reflect::Frame f(attached);
        f.arg(0, ev).arg(1, comp).arg_bool(5, true).arg_bool(6, true).arg_bool(7, true);
        ok = f.invoke(cdo);
    } else if (is2d) {
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
    std::string out = std::format("fx: hooks spawn={} deact={} sound={} stop={} | host captured spawn={} deact={} sound={} stop={} | client replayed spawn={} fail={} deact={} sound={} stop={} localBlocked fx={} sound={} | attachedSounds={} | emitters hook={} captured={} localBlocked={}{}\n",
                                  g_spawn_ok, g_deact_ok, g_sound_ok, g_stop_ok, g_cap_spawn, g_cap_deact, g_cap_sound, g_cap_stop, g_rep_spawn, g_rep_fail, g_rep_deact, g_rep_sound,
                                  g_rep_stop, g_local_blocked, g_local_sound_blocked, g_attached, g_em_ok, g_cap_emitter, g_local_emitter_blocked, g_block ? "" : " (blocking off)");
    for (const auto& [p, n] : g_blocked_ps) out += std::format("  local {} x{}\n", p.substr(p.rfind('/') + 1), n);
    for (const auto& [k, n] : g_em_kinds) out += std::format("  emitter {} x{}\n", k.substr(k.rfind('/') + 1), n);
    for (const auto& [k, n] : g_attached_kinds) out += std::format("  attached {} x{}\n", k.substr(k.rfind('/') + 1), n);
    return out;
}

} // namespace elmp::coop::fx
