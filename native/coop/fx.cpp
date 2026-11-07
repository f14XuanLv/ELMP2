#include "coop/fx.h"
#include "coop/ability.h"
#include "coop/causality.h"
#include "coop/entities/companion.h"
#include "coop/feedback_context.h"

#include "guard/guard.h"

#include "engine/handle.h"
#include "engine/frame.h"
#include "engine/reflect.h"

#include "base/hook.h"
#include "base/log.h"

#include <cstring>
#include <cmath>
#include <format>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace elmp::coop::fx {

namespace {

using RC::Unreal::UObject;

// UBaseFXComponent::SpawnFX_Implementation (IDA 0x7D0600, called by execSpawnFX through vtable +1024):
// (this, PS, ESpineBone, FName CustomBone, const FTransform* Offset, bAttach, bFollowBoneRotation,
//  int32 TranslucencySortPriority, EPSCPoolMethod, bRegisterAsRunningFX) → UParticleSystemComponent*
using SpawnFn = UObject*(__fastcall*)(UObject*, UObject*, std::uint8_t, std::uint64_t, const std::uint8_t*, bool, bool, std::int32_t, std::uint8_t, bool);
using DeactivateFn = void(__fastcall*)(UObject*);
// UFMODBlueprintStatics::PlayEventAtLocation (IDA 0x78D570): (WorldContext, UFMODEvent*, const FTransform&,
// bAutoPlay) → FFMODEventInstance (one pointer, returned in rax). This hook covers the location path;
// PlayEvent2D (IDA 0x78D520) forwards here with the identity transform. Attached/looping event
// lifecycles remain separate coverage gaps.
using SoundFn = std::uint64_t(__fastcall*)(UObject*, UObject*, const std::uint8_t*, bool);
// PlayEventAttached (0x78D7A0): counted only (attached / looping sounds are not replicated yet)
using AttachedFn = UObject*(__fastcall*)(UObject*, UObject*, std::uint64_t, const float*, std::uint8_t, bool, bool, bool);
using StopFn = void(__fastcall*)(UObject*, std::uint8_t);
// UGameplayStatics::SpawnEmitterAttached (IDA 0x25463A0) [E-IDA]:
// (PS, AttachToComponent, AttachPointName, Location*, Rotation*, Scale*, EAttachLocation, bAutoDestroy,
//  EPSCPoolMethod, bAutoActivate) → UParticleSystemComponent*
// 已核实的附着粒子入口：USpineFXComponent::SpawnFX（0x7D0600）内部就转调它，而 Spine 的两个动画通知
// 生成函数（0x7D0E60 / 0x7D1380，只被 vtable 引用）与蓝图直调都直接走这里 —— P6 只钩 SpawnFX 时漏的正是这些。
using EmAttachFn = UObject*(__fastcall*)(UObject*, UObject*, std::uint64_t, const float*, const float*, const float*, std::uint8_t, bool, std::uint8_t, bool);

SpawnFn o_spawn{};
DeactivateFn o_deactivate{};
SoundFn o_sound{};
AttachedFn o_attached{};
StopFn o_stop{};
EmAttachFn o_em_attach{};
std::uint64_t g_attached = 0, g_local_sound_blocked = 0;

Sinks g_sinks;
bool g_host = false, g_client = false, g_replaying = false;
bool g_spawn_ok = false, g_deact_ok = false, g_sound_ok = false, g_stop_ok = false, g_tried = false;
bool g_em_ok = false, g_in_spawn = false;
std::uint64_t g_cap_spawn = 0, g_cap_deact = 0, g_cap_sound = 0, g_cap_stop = 0;
std::uint64_t g_cap_emitter = 0, g_local_emitter = 0;
std::uint64_t g_attach_failed = 0;
std::uint64_t g_early_overflow = 0, g_emitter_inactive = 0;
std::string g_attach_failure;
std::string g_feedback_failure;
std::uint64_t g_feedback_fx = 0, g_feedback_snd = 0, g_feedback_blocked = 0;
std::uint64_t g_feedback_sound_instance = 0, g_feedback_sound_empty = 0;
ObjHandle g_widget_class;
std::uint64_t g_ui_sound_local = 0;
void feedback_fault(std::string why) { if (g_feedback_failure.empty()) g_feedback_failure = std::move(why); }
// Identity of the last captured spawn, not a whitelist of particle assets or all Ability actors.
// Keep the PSC's provenance through native Stop tails; overwrite it on every captured pool reuse.
std::unordered_map<UObject*, ObjHandle> g_native_ability_psc;
std::uint64_t g_native_ability_fx = 0, g_native_psc_overflow = 0;
constexpr std::size_t kNativePscCapacity = 4096;
// Only Pool=None replay results enter this set. Weak serial invalidation and explicit retire remove
// entries; native suppressed PSCs (including Companion.FX) never become owned here.
std::unordered_set<ObjHandle> g_remote_psc;
std::unordered_set<ObjHandle> g_retiring;
std::uint64_t g_retired = 0, g_retire_failed = 0, g_retire_expired = 0, g_remote_overflow = 0;
constexpr std::size_t kRemotePscCapacity = 1024;
bool remote_capacity() {
    std::erase_if(g_remote_psc, [](const auto& h) { return !h.valid(); });
    if (g_remote_psc.size() < kRemotePscCapacity) return true;
    ++g_remote_overflow;
    return false;
}
UObject* remember_remote(UObject* psc) {
    std::erase_if(g_remote_psc, [](const auto& h) { return !h.valid(); });
    if (ue::alive(psc)) g_remote_psc.insert(ObjHandle::of(psc));
    return psc;
}
void reap_retired(std::int64_t now_us);
std::uint64_t g_rep_spawn = 0, g_rep_fail = 0, g_rep_sound = 0, g_rep_stop = 0, g_rep_deact = 0, g_local_blocked = 0, g_rep_emitter = 0;
std::unordered_map<std::string, std::uint64_t> g_blocked_ps; // Dev: which local FX were suppressed (first 32 kinds)
// 会话开始前就 spawn 的持续特效（见 record_early）；句柄化，避免资产/组件被卸载后悬空
struct EarlyFx {
    ObjHandle owner, ps, psc;
    net::proto::Fx msg;
    std::string custom;
    std::string attach_component, attach_socket;
};

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

UObject* fx_owner(UObject* o); // 定义在 emitter 钩子旁
UObject* source_actor(UObject* o) {
    static auto* actor = reflect::find_class(L"/Script/Engine.Actor");
    for (int i = 0; ue::alive(o) && actor && i < 8; ++i, o = o->GetOuterPrivate())
        if (o->GetClassPrivate()->IsChildOf(actor)) return o;
    return nullptr;
}
bool native_fx_spawn(UObject* source, UObject* psc) {
    // A pooled PSC's UObject serial survives its next activation. Revoke before classifying this one.
    g_native_ability_psc.erase(psc);
    // Effect Apply/Finalize is never executed on Client. Its output cannot be delegated to the
    // replicated Ability's private writer, even when that Ability is the outer Actor.
    if (feedback_context::owns(source) || feedback_context::effect_scope()) return false;
    if (!ability::native_writer(source_actor(source))) return false;
    ++g_native_ability_fx;
    if (ue::alive(psc)) {
        if (g_native_ability_psc.size() >= kNativePscCapacity)
            std::erase_if(g_native_ability_psc, [](const auto& p) { return !p.second.valid(); });
        if (g_native_ability_psc.size() < kNativePscCapacity) g_native_ability_psc.emplace(psc, ObjHandle::of(psc));
        else ++g_native_psc_overflow;
    }
    return true;
}

// Cold emitter paths only. Native schema expires with its class/function handles; Blueprint FX
// properties are keyed by class serial and evicted when that class expires. No component pointer
// is retained: a rebuilt actor/component must resolve its identity again.
struct AttachSchema {
    ObjHandle actor, component, scene;
    reflect::PropRef root, location, rotation, scale;
    reflect::FuncRef components, component_owner;
    bool live() const { return actor.valid() && component.valid() && scene.valid() && components.handle.valid() && component_owner.handle.valid(); }
};
std::optional<AttachSchema> g_attach_schema;
const AttachSchema* attachment_schema() {
    if (g_attach_schema && g_attach_schema->live()) return &*g_attach_schema;
    g_attach_schema.reset();
    AttachSchema s;
    auto* a = reflect::find_class(L"/Script/Engine.Actor");
    auto* c = reflect::find_class(L"/Script/Engine.ActorComponent");
    auto* sc = reflect::find_class(L"/Script/Engine.SceneComponent");
    reflect::Resolver ar("emitter Actor", a), sr("emitter SceneComponent", sc), cr("emitter ActorComponent", c);
    s.root = ar.prop(L"RootComponent", reflect::Kind::Object);
    s.components = ar.func(L"K2_GetComponentsByClass", 1);
    s.component_owner = cr.func(L"GetOwner", 0);
    s.location = sr.prop(L"RelativeLocation", reflect::Kind::Struct, 12);
    s.rotation = sr.prop(L"RelativeRotation", reflect::Kind::Struct, 12);
    s.scale = sr.prop(L"RelativeScale3D", reflect::Kind::Struct, 12);
    if (!ar.ok() || !sr.ok() || !cr.ok()) return nullptr;
    s.actor = ObjHandle::of(a); s.component = ObjHandle::of(c); s.scene = ObjHandle::of(sc);
    g_attach_schema = s;
    return &*g_attach_schema;
}
bool actor_components(const AttachSchema& s, UObject* actor, std::vector<ObjHandle>& out) {
    reflect::Frame f(s.components);
    f.arg(0, s.component.get());
    if (!f.invoke(actor)) return false;
    const auto* ar = static_cast<const reflect::RawArray*>(f.raw(s.components.ret));
    if (!ar || ar->num < 0 || ar->num > 2048 || ar->max < ar->num || (ar->num && !ar->data)) return false;
    const auto* items = reinterpret_cast<UObject* const*>(ar->data);
    for (int i = 0; i < ar->num; ++i) if (ue::alive(items[i])) out.push_back(ObjHandle::of(items[i]));
    return true;
}
bool component_path(UObject* actor, UObject* component, std::string& path) {
    if (!ue::alive(actor) || !ue::alive(component)) return false;
    auto* outer = component->GetOuterPrivate();
    for (int i = 0; outer && outer != actor && i < 16; ++i) outer = outer->GetOuterPrivate();
    if (outer != actor) return false; // e.g. an Ability's private component cannot be mapped onto its Pawn
    path = log::utf8(component->GetPathName(actor));
    return !path.empty() && path.size() <= net::proto::kMaxName;
}
UObject* named_component(const AttachSchema& s, UObject* actor, const std::string& path) {
    if (path.empty() || path.size() > net::proto::kMaxName) return nullptr;
    std::vector<ObjHandle> items;
    if (!actor_components(s, actor, items)) return nullptr;
    UObject* found = nullptr;
    for (const auto& h : items) {
        std::string current;
        auto* c = h.get();
        if (component_path(actor, c, current) && current == path) {
            if (found) return nullptr; // ambiguous identity is not a root fallback
            found = c;
        }
    }
    return found;
}
UObject* companion_fx(UObject* actor) {
    if (!ue::alive(actor)) return nullptr;
    static std::unordered_map<ObjHandle, reflect::PropRef> properties;
    std::erase_if(properties, [](const auto& e) { return !e.first.valid(); });
    const auto type = ObjHandle::of(actor->GetClassPrivate());
    auto it = properties.find(type);
    if (it == properties.end()) {
        auto p = reflect::Resolver("emitter Companion", actor->GetClassPrivate()).prop(L"FX", reflect::Kind::Object);
        if (!p.ok()) return nullptr;
        it = properties.emplace(type, p).first;
    }
    auto* psc = it->second.object(actor);
    return entities::companion_owns_fx(actor, psc) ? psc : nullptr;
}
struct BoneSchema {
    ObjHandle type;
    reflect::PropRef followers, rotated, feet;
    reflect::FuncRef attach;
};
std::optional<BoneSchema> g_bone_schema;
const BoneSchema* bone_schema() {
    if (g_bone_schema && g_bone_schema->type.valid() && g_bone_schema->attach.handle.valid()) return &*g_bone_schema;
    g_bone_schema.reset();
    auto* type = reflect::find_class(L"/Script/SpineExtensionPlugin.SpineBoneComponent");
    reflect::Resolver r("emitter SpineBone", type);
    BoneSchema s;
    s.type = ObjHandle::of(type);
    s.followers = r.prop(L"BoneFollowers", reflect::Kind::Map);
    s.rotated = r.prop(L"RotatedBoneFollowers", reflect::Kind::Map);
    s.feet = r.prop(L"SceneFeetComponent", reflect::Kind::Object);
    s.attach = r.func(L"GetAttachComponent", 2);
    if (!r.ok()) return nullptr;
    g_bone_schema = s;
    return &*g_bone_schema;
}
bool capture_attachment(const AttachSchema& s, UObject* owner, UObject* attach, Spawn& out) {
    using net::proto::FxAttach;
    auto kind = [&](FxAttach k) { out.msg.attach_kind = static_cast<std::uint8_t>(k); };
    if (s.root.object(owner) == attach) { kind(FxAttach::Root); return true; }
    if (entities::companion_owns_fx(owner, attach)) { kind(FxAttach::CompanionFx); return true; }
    std::vector<ObjHandle> items;
    if (!actor_components(s, owner, items)) return false;
    const auto* bs = bone_schema();
    for (const auto& h : items) {
        auto* c = h.get();
        if (!c || !bs || c->GetClassPrivate() != bs->type.get()) continue; // no unverified BP override
        bool found = bs->feet.object(c) == attach, follow = false;
        std::uint8_t bone = 10;
        for (int rotated = 0; !found && rotated < 2; ++rotated) {
            for (const auto& pair : reflect::map_pairs(c, rotated ? bs->rotated : bs->followers)) {
                UObject* value{};
                std::memcpy(&value, pair.value, sizeof(value));
                if (value == attach) { bone = *pair.key; follow = rotated != 0; found = true; break; }
            }
        }
        if (found) {
            if (!net::proto::valid_attach_bone(bone) || !component_path(owner, c, out.attach_component)) return false;
            kind(FxAttach::SpineBone); out.msg.bone = bone;
            if (follow) out.msg.flags |= 2;
            return true;
        }
    }
    // A named component must actually belong to this owner. A generated bone follower which is
    // absent from the proven native maps is unsupported; never guess from its generated suffix.
    if (ue::class_name_of(attach) == L"SpineBoneFollowerComponent") return false;
    if (!component_path(owner, attach, out.attach_component)) return false;
    kind(FxAttach::Component);
    return true;
}
UObject* resolve_attachment(const AttachSchema& s, UObject* actor, const net::proto::Fx& m, const std::string& path) {
    using net::proto::FxAttach;
    switch (static_cast<FxAttach>(m.attach_kind)) {
    case FxAttach::Root: return s.root.object(actor);
    case FxAttach::CompanionFx: return companion_fx(actor);
    case FxAttach::Component: return named_component(s, actor, path);
    case FxAttach::SpineBone: {
        auto* c = named_component(s, actor, path);
        const auto* bs = bone_schema();
        if (!c || !bs || c->GetClassPrivate() != bs->type.get() || !s.root.object(actor) ||
            c->HasAnyFlags(static_cast<RC::Unreal::EObjectFlags>(RC::Unreal::RF_ClassDefaultObject | RC::Unreal::RF_ArchetypeObject))) return nullptr;
        reflect::Frame owner(s.component_owner);
        if (!net::proto::valid_attach_bone(m.bone) || !owner.invoke(c) || owner.ret<UObject*>() != actor) return nullptr;
        // E-IDA 7CEEE0 caches enum followers. 7CED20 initializes a new follower by ticking only its
        // component transform; it does not move Actor or settle gameplay. Never call ByName per FX.
        reflect::Frame f(bs->attach);
        f.arg(0, m.bone).arg_bool(1, m.flags & 2);
        return f.invoke(c) ? f.ret<UObject*>() : nullptr;
    }
    default: return nullptr;
    }
}

// Registered running particles (flags bit2) can predate hosting. This cache covers those particle
// instances, not material effects such as BattleColor. Deactivate and weak-handle invalidation retire them.
// 所以钩子在 on_unreal_init 就装好，没有会话时只把这类持续特效记在 g_early 里，
// Host 一开会话就把它们补进在场集合并发给（晚加入的）客户端。
std::vector<EarlyFx> g_early;
std::unordered_map<std::string, std::uint64_t> g_early_ps; // Dev: 会话前出现的持续特效种类（审计用）

void record_early(UObject* owner, UObject* ps, UObject* psc, const net::proto::Fx& m, const std::string& custom,
                  const std::string& attach_component = {}, const std::string& attach_socket = {}) {
    std::erase_if(g_early, [psc](const EarlyFx& e) { return !e.owner.get() || !e.ps.get() || !active(e.psc.get()) || e.psc.get() == psc; });
    if (!ue::alive(owner) || !ue::alive(ps) || !active(psc)) return;
    if (g_early.size() >= 128) return void(++g_early_overflow);
    if (g_early_ps.size() < 32) ++g_early_ps[path_of(ps)];
    g_early.push_back({ObjHandle::of(owner), ObjHandle::of(ps), ObjHandle::of(psc), m, custom, attach_component, attach_socket});
}

std::size_t take_early_impl(std::vector<Spawn>& out) {
    for (auto& e : g_early) {
        auto* ps = e.ps.get();
        if (!ps || !e.owner.get() || !active(e.psc.get())) continue; // stopped/pooled/destroyed FX are not an initial state
        Spawn s;
        s.owner = e.owner.get();
        s.ps = ps;
        s.psc = e.psc.get();
        s.msg = e.msg;
        s.custom_bone = e.custom;
        s.attach_component = e.attach_component;
        s.attach_socket = e.attach_socket;
        out.push_back(s);
    }
    const auto n = out.size();
    g_early.clear();
    return n;
}

UObject* __fastcall d_spawn(UObject* comp, UObject* ps, std::uint8_t bone, std::uint64_t custom, const std::uint8_t* xf, bool attach, bool follow, std::int32_t sort,
                            std::uint8_t pool, bool reg) {
    struct SpawnScope { bool previous = g_in_spawn; SpawnScope() { g_in_spawn = true; } ~SpawnScope() { g_in_spawn = previous; } } spawn_scope;
    auto* psc = o_spawn(comp, ps, bone, custom, xf, attach, follow, sort, pool, reg);
    if (!ue::on_game_thread() || !comp) return psc;
    if (native_fx_spawn(comp, psc)) return psc;
    const bool feedback = feedback_context::owns(comp);
    if (feedback_context::effect_scope() && !feedback) {
        feedback_fault("feedback SpawnFX source has no proven owner/attachment");
        if (g_client && !g_replaying && ue::alive(psc)) hide_now(psc);
        return psc;
    }
    auto* owner = comp->GetOuterPrivate();
    if (auto* resolved = fx_owner(owner)) {
        // BaseFX replay resolves the target Actor's component. A private Ability component is not
        // equivalent, even if its Owner happens to be that Actor; no proven wire identity exists yet.
        if (feedback && resolved != source_actor(comp)) {
            feedback_fault("feedback SpawnFX requires an unsupported private Ability component");
            if (g_client && !g_replaying && ue::alive(psc)) hide_now(psc);
            return psc;
        }
        owner = resolved;
    }
    const bool have = ps && xf && (g_host || g_client || reg); // 没有会话时只关心持续特效（reg），别为别的付代价
    Spawn s;
    if (have) {
        s.owner = owner;
        s.ps = ps;
        s.psc = psc;
        s.msg.bone = bone;
        read_xf(xf, s.msg.xf);
        s.msg.flags = (attach ? 1 : 0) | (follow ? 2 : 0) | (reg ? 4 : 0) | (feedback ? 32 : 0);
        s.msg.sort = sort;
        s.msg.pool = pool;
        ue::FName n;
        std::memcpy(&n, &custom, sizeof(n));
        s.custom_bone = ue::utf8(n);
    }
    if (g_host && g_sinks.spawn && have) {
        ++g_cap_spawn;
        if (feedback) ++g_feedback_fx;
        g_sinks.spawn(s);
    } else if (!g_client && have && (s.msg.flags & 4)) {
        record_early(owner, ps, psc, s.msg, s.custom_bone); // 会话前的持续特效：留给 Host 补发
    } else if (g_client && owner && (feedback || guard::marked(owner)) && ue::alive(psc)) {
        if (!g_replaying) {
            ++g_local_blocked;
            if (feedback) ++g_feedback_blocked;
            if (ps && g_blocked_ps.size() < 32) ++g_blocked_ps[path_of(ps)];
            hide_now(psc);
        }
    }
    return psc;
}

// Generic FX attribution only. Registered native Ability writers have already returned above:
// folding their private component to the Invoker loses the very attachment the original BP owns.
UObject* fx_owner(UObject* o) {
    static ue::UClass* actor = reflect::find_class(L"/Script/Engine.Actor");
    static ue::UClass* ability = reflect::find_class(L"/Script/Zenith.Ability");
    static reflect::FuncRef get_owner;
    if (actor && !get_owner.ok()) get_owner = reflect::Resolver("Actor", actor).func(L"GetOwner", 0);
    for (int i = 0; o && actor && i < 8; ++i, o = o->GetOuterPrivate()) {
        if (!o->GetClassPrivate()->IsChildOf(actor)) continue;
        if (ability && o->GetClassPrivate()->IsChildOf(ability)) {
            if (get_owner.ok()) {
                reflect::Frame f(get_owner);
                if (f.invoke(o))
                    if (auto* owner = f.ret<UObject*>()) return owner;
            }
            return nullptr;
        }
        return o;
    }
    return nullptr;
}

UObject* __fastcall d_em_attach(UObject* ps, UObject* attach_to, std::uint64_t point, const float* loc, const float* rot, const float* scale, std::uint8_t loc_type,
                                bool autodestroy, std::uint8_t pool, bool activate) {
    auto* psc = o_em_attach(ps, attach_to, point, loc, rot, scale, loc_type, autodestroy, pool, activate);
    if (!ue::on_game_thread() || g_in_spawn || !ps || !attach_to) return psc;
    if (native_fx_spawn(attach_to, psc)) return psc;
    const bool feedback = feedback_context::owns(attach_to);
    if (feedback_context::effect_scope() && !feedback) {
        feedback_fault("feedback emitter source has no proven owner/attachment");
        if (g_client && !g_replaying && ue::alive(psc)) hide_now(psc);
        return psc;
    }
    auto* owner = fx_owner(attach_to);
    if (!owner) return psc;
    // The exact Companion.FX PSC has a snapshot state writer. The Blueprint assigns FX after this
    // native call returns, so host delivery also repeats this test once that assignment is visible.
    if (entities::companion_owns_fx(owner, psc)) return psc;
    if ((g_host && g_sinks.spawn) || !g_client) {
        auto rejected = [&](const char* why) { ++g_attach_failed; g_attach_failure = why; if (feedback) feedback_fault(why); };
        if (!active(psc)) { ++g_emitter_inactive; return psc; }
        const auto* schema = attachment_schema();
        if (!schema || !ue::alive(psc)) { rejected("PSC relative transform schema/result"); return psc; }
        Spawn s;
        s.owner = owner;
        s.ps = ps;
        s.psc = psc;
        s.msg.flags = 8 | 4 | 1 | (feedback ? 32 : 0); // active emitter baseline until stopped/invalid
        s.msg.sort = loc_type;
        s.msg.pool = pool;
        s.msg.emitter_flags = (autodestroy ? 1 : 0) | (activate ? 2 : 0);
        // Vanilla has already converted KeepWorldPosition / Snap scale using the original socket.
        // Send this result and replay KeepRelativeOffset, never reinterpret world inputs at a new root.
        std::memcpy(&s.msg.xf[0], schema->rotation.at<float>(psc), 12);
        std::memcpy(&s.msg.xf[4], schema->location.at<float>(psc), 12);
        std::memcpy(&s.msg.xf[7], schema->scale.at<float>(psc), 12);
        for (float v : s.msg.xf) if (!std::isfinite(v) || std::abs(v) > 1e6f) { rejected("nonfinite/range relative transform"); return psc; }
        ue::FName socket;
        std::memcpy(&socket, &point, sizeof(socket));
        s.attach_socket = ue::utf8(socket);
        if (s.attach_socket.size() > net::proto::kMaxName || loc_type > 3) { rejected("socket length/location rule"); return psc; }
        if (!capture_attachment(*schema, owner, attach_to, s)) { rejected("unresolved exact attachment identity"); return psc; }
        if (g_host && g_sinks.spawn) { ++g_cap_emitter; if (feedback) ++g_feedback_fx; g_sinks.spawn(s); }
        else record_early(owner, ps, psc, s.msg, {}, s.attach_component, s.attach_socket);
    } else if (g_client && ue::alive(psc) && (feedback || guard::marked(owner))) {
        // Host results are the only writer for particles on presented owners.
        if (!g_replaying) {
            ++g_local_emitter;
            if (feedback) ++g_feedback_blocked;
            hide_now(psc);
        }
    }
    return psc;
}

void __fastcall d_deactivate(UObject* psc) {
    if (psc && ue::on_game_thread()) std::erase_if(g_early, [psc](const EarlyFx& e) { return e.psc.get() == psc; });
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
        // A gameplay delegate may synchronously refresh UI (e.g. RestoreMP -> gauge full).
        // Its Widget-owned sound remains local UI, regardless of an enclosing Effect scope.
        if(g_host||g_client){if(!g_widget_class.valid())g_widget_class=ObjHandle::of(reflect::find_class(L"/Script/UMG.Widget"));if(auto* widget=g_widget_class.get();ctx&&widget&&ctx->IsA(static_cast<ue::UClass*>(widget))){++g_ui_sound_local;return o_sound(ctx,ev,xf,autoplay);}}
        auto* target = feedback_context::sound_target(ctx);
        const bool feedback = target != nullptr;
        // Contact -> Knockback can synchronously cancel a native Ability. Its private OnStop
        // sounds retain their existing writer; an enclosing Contact alone proves no sound source.
        if ((!target && feedback_context::effect_scope()) || (feedback && (!xf || !autoplay))) {
            const auto label=[](UObject* o){return o?log::utf8(o->GetFullName()):std::string{"null"};};
            feedback_fault(std::format("feedback sound context: source={} context={} event={} target={} transform={} autoplay={}",label(feedback_context::effect_source()),label(ctx),label(ev),label(target),xf!=nullptr,autoplay));
            if (g_client && !g_replaying) return ++g_feedback_blocked, 0;
            return o_sound(ctx, ev, xf, autoplay);
        }
        auto* owner = target ? target : sound_owner(ctx);
        if (g_host && g_sinks.sound) {
            if (owner && xf && autoplay) {
                Sound s;
                s.owner = owner; s.event = ev;
                read_xf(xf, s.xf);
                s.is2d = s.xf[4] == 0 && s.xf[5] == 0 && s.xf[6] == 0; // existing PlayEvent2D identity convention
                s.flags = 2 | (feedback ? 1 : 0);
                ++g_cap_sound;
                if (feedback) ++g_feedback_snd;
                g_sinks.sound(s);
            }
        } else if (g_client && !g_replaying) {
            if (owner && (feedback || guard::marked(owner))) {
                if (feedback) ++g_feedback_blocked;
                return ++g_local_sound_blocked, 0; // only proven feedback also suppresses the local P1
            }
        }
    }
    return o_sound(ctx, ev, xf, autoplay);
}

UObject* __fastcall d_attached(UObject* ev, UObject* parent, std::uint64_t socket, const float* at, std::uint8_t rule, bool stop, bool play, bool destroy) {
    if (ue::on_game_thread()) ++g_attached;
    return o_attached(ev, parent, socket, at, rule, stop, play, destroy);
}

void __fastcall d_stop(UObject* ability, std::uint8_t reason) {
    elmp::coop::ability::Identity before;
    if (ue::on_game_thread()) elmp::coop::ability::identity(ability, before);
    if (ue::on_game_thread() && !causality::allow_stop(ability, before, reason)) return;
    if (ue::on_game_thread()) causality::stop_requested(ability, before, reason);
    if (g_host && g_sinks.ability_stop && ability && ue::on_game_thread()) ++g_cap_stop, g_sinks.ability_stop(ability, reason);
    if (!ue::on_game_thread()) { o_stop(ability, reason); return; }
    elmp::coop::ability::NativeScope scope(ability);
    o_stop(ability, reason);
    elmp::coop::ability::stopped(ability, before);
}

void install_hooks() {
    if (g_tried) return;
    g_tried = true;
    g_spawn_ok = hook::install_manifest("SpawnFXImpl", reinterpret_cast<void*>(&d_spawn), reinterpret_cast<void**>(&o_spawn));
    g_em_ok = hook::install_manifest("SpawnEmitterAttached", reinterpret_cast<void*>(&d_em_attach), reinterpret_cast<void**>(&o_em_attach));
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

void install() {
    feedback_context::install(); // on_unreal_init; phase registration must precede frame iteration
    install_hooks();
    static bool retirement_installed = false;
    if (!retirement_installed) {
        retirement_installed = true;
        // Unlike the Client role tick this phase remains eligible after disconnect until every tail
        // completed or its World/owner expired. No calls occur with an empty retirement set.
        frame::add_phase("fx_retire", [](const frame::Tick& t) { reap_retired(t.now_us); }, [] { return !g_retiring.empty(); });
    }
}

std::size_t take_early(std::vector<Spawn>& out) { return take_early_impl(out); }

bool active(ue::UObject* psc) {
    // The property belongs to the native ActorComponent class; component lifetime is checked before every read.
    static reflect::PropRef enabled = reflect::Resolver("ActorComponent", reflect::find_class(L"/Script/Engine.ActorComponent")).prop(L"bIsActive", reflect::Kind::Bool);
    return ue::alive(psc) && enabled.ok() && enabled.read_bool(psc);
}

bool native_ability_owned(ue::UObject* psc) {
    const auto it = g_native_ability_psc.find(psc);
    if (it == g_native_ability_psc.end()) return false;
    if (it->second.get() == psc) return true;
    g_native_ability_psc.erase(it);
    return false;
}

bool host_enable(const Sinks& s) {
    install();
    if (!feedback_context::enable()) return false;
    g_sinks = s;
    g_host = g_spawn_ok && g_sound_ok && g_deact_ok && g_stop_ok && g_em_ok;
    return g_host;
}

void host_disable() {
    g_host = false; g_sinks = {}; g_early.clear(); g_early_ps.clear(); g_native_ability_psc.clear();
    if (!g_client) feedback_context::disable();
    g_feedback_failure.clear();
}

bool client_enable() {
    install();
    if (!feedback_context::enable()) return false;
    g_early.clear(); // client-side effects cannot seed a later hosting session
    g_client = g_spawn_ok && g_em_ok && g_sound_ok;
    return g_client;
}

void client_disable() {
    g_client = false; g_early.clear(); g_native_ability_psc.clear();
    if (!g_host) feedback_context::disable();
    g_feedback_failure.clear();
}
std::string fault() { return g_feedback_failure.empty() ? feedback_context::fault() : g_feedback_failure; }

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
    if (!remote_capacity()) return fail("remote PSC capacity");
    reflect::Frame f(spawn);
    f.arg(0, ps).arg(1, m.bone).arg_name(2, custom_bone.empty() ? ue::FName() : ue::FName(log::wide(custom_bone).c_str(), RC::Unreal::FNAME_Add));
    if (auto* t = static_cast<std::uint8_t*>(f.raw(3))) write_xf(t, m.xf);
    f.arg_bool(4, m.flags & 1).arg_bool(5, m.flags & 2).arg(6, m.sort).arg(7, std::uint8_t{0}).arg_bool(8, m.flags & 4);
    g_replaying = true;
    const bool ok = f.invoke(comp);
    g_replaying = false;
    if (!ok) return fail("invoke");
    ++g_rep_spawn;
    return remember_remote(f.ret<ue::UObject*>());
}

ue::UObject* replay_emitter(ue::UObject* actor, const net::proto::Fx& m, const std::string& ps_path, const std::string& attach_comp, const std::string& socket, std::string* why) {
    auto fail = [&](const char* w) -> ue::UObject* {
        ++g_rep_fail;
        if (why) *why = w;
        return nullptr;
    };
    static ue::UClass* ps_cls = reflect::find_class(L"/Script/Engine.ParticleSystem");
    auto* ps = asset(ps_path);
    if (!ps || !ps_cls || !ps->GetClassPrivate()->IsChildOf(ps_cls)) return fail("not a ParticleSystem");
    static ue::UClass* gs_cls = reflect::find_class(L"/Script/Engine.GameplayStatics");
    static reflect::FuncRef spawn = reflect::Resolver("GameplayStatics", gs_cls).func(L"SpawnEmitterAttached", 10);
    auto* cdo = reflect::find_object(L"/Script/Engine.Default__GameplayStatics");
    const auto* schema = attachment_schema();
    if (!ue::alive(actor) || !cdo || !schema || !spawn.ok()) return fail("SpawnEmitterAttached schema/owner");
    if (socket.size() > net::proto::kMaxName || (m.attach_socket && socket.empty())) return fail("missing/oversize socket");
    auto* attach = resolve_attachment(*schema, actor, m, attach_comp);
    if (!ue::alive(attach) || !attach->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(schema->scene.get()))) return fail("exact attachment unresolved");
    if (!remote_capacity()) return fail("remote PSC capacity");
    reflect::Frame f(spawn);
    f.arg(0, ps).arg(1, attach).arg_name(2, socket.empty() ? ue::FName() : ue::FName(log::wide(socket).c_str(), RC::Unreal::FNAME_Add));
    if (auto* p = static_cast<float*>(f.raw(3))) p[0] = m.xf[4], p[1] = m.xf[5], p[2] = m.xf[6];
    if (auto* p = static_cast<float*>(f.raw(4))) p[0] = m.xf[0], p[1] = m.xf[1], p[2] = m.xf[2];
    if (auto* p = static_cast<float*>(f.raw(5))) p[0] = m.xf[7], p[1] = m.xf[8], p[2] = m.xf[9];
    // EAttachLocation is a byte enum. Its original rule is already represented by the actual
    // relative transform; PoolMethod=None gives this network activation its own PSC lifetime.
    f.arg(6, std::uint8_t{0}).arg_bool(7, m.emitter_flags & 1).arg(8, std::uint8_t{0}).arg_bool(9, m.emitter_flags & 2);
    g_replaying = true;
    const bool ok = f.invoke(cdo);
    g_replaying = false;
    if (!ok) return fail("invoke");
    ++g_rep_emitter;
    return remember_remote(f.ret<ue::UObject*>());
}

bool deactivate(ue::UObject* psc) {    static reflect::FuncRef d = reflect::Resolver("ActorComponent", reflect::find_class(L"/Script/Engine.ActorComponent")).func(L"Deactivate", 0);
    if (!ue::alive(psc) || !d.ok()) return false;
    if (g_client) ++g_rep_deact;
    return reflect::Frame(d).invoke(psc);
}

bool retire(ue::UObject* psc) {
    if (!ue::alive(psc)) return true;
    const auto h = ObjHandle::of(psc);
    if (!g_remote_psc.contains(h)) return ++g_retire_failed, false;
    if (g_retiring.contains(h)) return true;
    std::erase_if(g_retiring, [](const auto& old) { return !old.valid(); });
    if (!deactivate(psc)) return ++g_retire_failed, false;
    // g_remote_psc bounds all live replays, hence this subset cannot exceed 1024.
    g_retiring.insert(h);
    return true;
}

namespace {
void reap_retired(std::int64_t now_us) {
    static std::int64_t next_us = 0;
    if (now_us < next_us) return;
    next_us = now_us + 100'000;
    static reflect::FuncRef owner = reflect::Resolver("ActorComponent", reflect::find_class(L"/Script/Engine.ActorComponent")).func(L"GetOwner", 0);
    static reflect::FuncRef destroy = reflect::Resolver("ActorComponent", reflect::find_class(L"/Script/Engine.ActorComponent")).func(L"K2_DestroyComponent", 1);
    static reflect::FuncRef count = reflect::Resolver("ParticleSystemComponent", reflect::find_class(L"/Script/Engine.ParticleSystemComponent")).func(L"GetNumActiveParticles", 0);
    std::erase_if(g_retiring, [&](const ObjHandle& h) {
        auto* psc = h.get();
        if (!psc) { g_remote_psc.erase(h); ++g_retire_expired; return true; }
        // DeactivateSystem suppresses spawning but leaves active=true for existing particles.
        // Only Complete turns it off; zero CPU particles alone is not completion (e.g. GPU FX).
        if (active(psc)) return false;
        if (!owner.ok() || !destroy.ok() || !count.ok()) { ++g_retire_failed; return false; }
        reflect::Frame n(count);
        if (!n.invoke(psc)) { ++g_retire_failed; return false; }
        if (n.ret<std::int32_t>() != 0) return false;
        reflect::Frame o(owner);
        if (!o.invoke(psc)) { ++g_retire_failed; return false; }
        reflect::Frame d(destroy);
        d.arg(0, o.ret<ue::UObject*>());
        if (!d.invoke(psc)) { ++g_retire_failed; return false; }
        g_remote_psc.erase(h);
        ++g_retired;
        return true;
    });
}
} // namespace

bool play_sound(ue::UObject* actor, const std::string& event_path, const net::proto::Snd& msg, std::string* why) {
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
    if ((msg.flags & 1) && (at.ret < 0 || at.parms[at.ret].size != sizeof(std::uint64_t))) return fail("FMOD instance return schema");
    bool ok = false;
    std::uint64_t instance = 0;
    g_replaying = true;
    if (msg.flags & 2) {
        // Preserve the Host's original FTransform, including identity from PlayEvent2D. Do not
        // substitute the current/interpolated owner position for the point at which the hit occurred.
        reflect::Frame f(at);
        f.arg(0, actor).arg(1, ev).arg_bool(3, true);
        auto* dst = static_cast<std::uint8_t*>(f.raw(2));
        if (dst) { write_xf(dst, msg.xf); ok = f.invoke(cdo); if (ok) instance = f.ret<std::uint64_t>(); }
    } else if (msg.is2d) {
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
    if (msg.flags & 1) {
        // FMOD can legitimately return null for an inaudible World or unavailable bank. This
        // diagnoses actual native instance creation, not physical speaker output or a session fault.
        if (instance) ++g_feedback_sound_instance;
        else ++g_feedback_sound_empty;
    }
    ++g_rep_sound;
    return true;
}

bool stop_ability(ue::UObject* ability, std::uint8_t reason) {
    static reflect::FuncRef stop = reflect::Resolver("Ability", reflect::find_class(L"/Script/Zenith.Ability")).func(L"Stop", 1);
    if (!ue::alive(ability) || !stop.ok()) return false;
    // IsLaunched only recognizes native states 2/3; state 4 is still active while effects drain.
    // Host END_PLAY / a later Stop must be allowed to reach vanilla in that state too.
    static reflect::FuncRef active = reflect::Resolver("ReusableActor", reflect::find_class(L"/Script/Zenith.ReusableActor")).func(L"IsActive", 0);
    reflect::Frame l(active);
    if (!active.ok() || !l.invoke(ability) || !l.ret_bool()) return false;
    reflect::Frame f(stop);
    f.arg(0, reason);
    if (!f.invoke(ability)) return false;
    ++g_rep_stop;
    return true;
}

std::string describe() {
    std::string out = std::format("fx: hooks spawn={} deact={} sound={} stop={} emitter={} | host captured spawn={} deact={} sound={} stop={} emitter={} | client replayed spawn={} fail={} deact={} sound={} stop={} emitter={} localBlocked fx={} sound={} emitter={} | attachedSounds={}\n",
                                  g_spawn_ok, g_deact_ok, g_sound_ok, g_stop_ok, g_em_ok, g_cap_spawn, g_cap_deact, g_cap_sound, g_cap_stop,
                                  g_cap_emitter, g_rep_spawn, g_rep_fail, g_rep_deact, g_rep_sound, g_rep_stop, g_rep_emitter, g_local_blocked, g_local_sound_blocked, g_local_emitter, g_attached);
    out += std::format("  early persistent fx: {} overflow={}\n", g_early.size(), g_early_overflow);
    out += std::format("  emitter attachmentRejected={} inactiveAtSpawn={} last={}\n", g_attach_failed, g_emitter_inactive, g_attach_failure);
    out += std::format("  feedback fx={} sounds={} localSuppressed={} uiLocal={} fault={}\n", g_feedback_fx, g_feedback_snd, g_feedback_blocked,g_ui_sound_local, fault());
    out += std::format("  feedback FMOD replay instanceCreated={} nativeEmpty={}\n", g_feedback_sound_instance, g_feedback_sound_empty);
    out += feedback_context::describe();
    out += std::format("  native ability FX kept={} exactPSC={} capacityRejected={}\n", g_native_ability_fx, g_native_ability_psc.size(), g_native_psc_overflow);
    out += std::format("  remote ownedPSC={} retiring={} retired={} expired={} retireFailed={} capacityRejected={}\n", g_remote_psc.size(), g_retiring.size(), g_retired, g_retire_expired, g_retire_failed, g_remote_overflow);
    for (const auto& [p, n] : g_early_ps) out += std::format("  early {} x{}\n", p.substr(p.rfind('/') + 1), n);
    for (const auto& [p, n] : g_blocked_ps) out += std::format("  local {} x{}\n", p.substr(p.rfind('/') + 1), n);
    return out;
}

} // namespace elmp::coop::fx
