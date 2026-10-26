#include "coop/ability.h"

#include "engine/reflect.h"

#include "base/hook.h"
#include "base/log.h"

#include <cstring>
#include <format>

namespace elmp::coop::ability {

namespace {

using RC::Unreal::UObject;

struct Layout {
    reflect::PropRef cls, offset, spawn_bone, custom_spawn, follow_spawn, attach_bone, custom_attach, follow_attach, ffa;
    bool ok = false, tried = false;
};
Layout g_l;

const Layout& layout() {
    if (g_l.tried) return g_l;
    g_l.tried = true;
    reflect::Resolver r("AbilityDescription", static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.AbilityDescription")));
    g_l.cls = r.prop(L"AbilityClass", reflect::Kind::Any, 8);
    g_l.offset = r.prop(L"Offset", reflect::Kind::Struct, 48);
    g_l.spawn_bone = r.prop(L"SpawnBone", reflect::Kind::Any, 1);
    g_l.custom_spawn = r.prop(L"CustomSpawnBone", reflect::Kind::Name);
    g_l.follow_spawn = r.prop(L"bFollowSpawnBoneRotation", reflect::Kind::Bool);
    g_l.attach_bone = r.prop(L"AttachBone", reflect::Kind::Any, 1);
    g_l.custom_attach = r.prop(L"CustomAttachBone", reflect::Kind::Name);
    g_l.follow_attach = r.prop(L"bFollowAttachBoneRotation", reflect::Kind::Bool);
    g_l.ffa = r.prop(L"bFireAndForget", reflect::Kind::Bool);
    g_l.ok = r.ok();
    if (!g_l.ok) log::warn("ability: AbilityDescription layout: {}", r.failure());
    return g_l;
}

// FTransform (float build): FQuat Rotation, FVector Translation (+pad), FVector Scale3D (+pad)
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

using LaunchFn = UObject*(__fastcall*)(UObject*, const std::uint8_t*, const std::uint8_t*);
LaunchFn o_launch{};
void (*g_sink)(const Launch&) = nullptr;
bool g_installed = false, g_failed = false;
std::uint64_t g_captured = 0, g_replayed = 0, g_rejected = 0;

UObject* __fastcall d_launch(UObject* comp, const std::uint8_t* desc, const std::uint8_t* offset) {
    if (g_sink && desc && offset && ue::on_game_thread() && layout().ok) {
        auto* cls = *g_l.cls.at<UObject*>(desc);
        if (ue::alive(cls)) {
            Launch l;
            l.owner = comp ? comp->GetOuterPrivate() : nullptr;
            l.cls = log::utf8(cls->GetPathName());
            read_xf(g_l.offset.at<std::uint8_t>(desc), l.act.xf);
            read_xf(offset, l.act.xf2);
            l.act.spawn_bone = *g_l.spawn_bone.at<std::uint8_t>(desc);
            l.act.attach_bone = *g_l.attach_bone.at<std::uint8_t>(desc);
            l.custom_spawn = ue::utf8(*g_l.custom_spawn.at<ue::FName>(desc));
            l.custom_attach = ue::utf8(*g_l.custom_attach.at<ue::FName>(desc));
            l.act.flags = (g_l.follow_spawn.read_bool(desc) ? 1 : 0) | (g_l.follow_attach.read_bool(desc) ? 2 : 0) | (g_l.ffa.read_bool(desc) ? 4 : 0);
            ++g_captured;
            g_sink(l);
        }
    }
    return o_launch(comp, desc, offset);
}

} // namespace

bool host_enable(void (*sink)(const Launch&)) {
    if (!g_installed && !g_failed) {
        g_installed = hook::install_manifest("AbilityLaunchFromDesc", reinterpret_cast<void*>(&d_launch), reinterpret_cast<void**>(&o_launch));
        g_failed = !g_installed;
    }
    g_sink = g_installed ? sink : nullptr;
    return g_installed;
}

void host_disable() { g_sink = nullptr; }

bool replay(ue::UObject* enemy, const net::proto::Act& act, const std::string& cls_path, const std::string& custom_spawn, const std::string& custom_attach,
            std::string* why) {
    auto fail = [&](const char* w) {
        ++g_rejected;
        if (why) *why = w;
        return false;
    };
    if (!layout().ok) return fail("layout");
    static ue::UClass* ability_base = reflect::find_class(L"/Script/Zenith.Ability");
    auto* cls = static_cast<ue::UClass*>(reflect::load_object(log::wide(cls_path)));
    if (!cls || !ability_base || !cls->IsChildOf(ability_base)) return fail("not an Ability class");
    static ObjHandle comp_cls = ObjHandle::of(reflect::find_object(L"/Script/Zenith.AbilityComponent"));
    static reflect::FuncRef by_class, launch;
    if (!launch.ok()) {
        by_class = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"GetComponentByClass", 1);
        launch = reflect::Resolver("AbilityComponent", reflect::find_class(L"/Script/Zenith.AbilityComponent")).func(L"LaunchAbilityFromDescription", 2);
    }
    if (!launch.ok() || !by_class.ok()) return fail("LaunchAbilityFromDescription");
    reflect::Frame c(by_class);
    c.arg(0, comp_cls.get());
    auto* comp = c.invoke(enemy) ? c.ret<ue::UObject*>() : nullptr;
    if (!ue::alive(comp)) return fail("no AbilityComponent");
    reflect::Frame f(launch);
    auto* desc = static_cast<std::uint8_t*>(f.raw(0));
    auto* off = static_cast<std::uint8_t*>(f.raw(1));
    if (!desc || !off) return fail("frame");
    *g_l.cls.at<ue::UClass*>(desc) = cls;
    write_xf(g_l.offset.at<std::uint8_t>(desc), act.xf);
    write_xf(off, act.xf2);
    *g_l.spawn_bone.at<std::uint8_t>(desc) = act.spawn_bone;
    *g_l.attach_bone.at<std::uint8_t>(desc) = act.attach_bone;
    *g_l.custom_spawn.at<ue::FName>(desc) = custom_spawn.empty() ? ue::FName() : ue::FName(log::wide(custom_spawn).c_str(), RC::Unreal::FNAME_Add);
    *g_l.custom_attach.at<ue::FName>(desc) = custom_attach.empty() ? ue::FName() : ue::FName(log::wide(custom_attach).c_str(), RC::Unreal::FNAME_Add);
    g_l.follow_spawn.write_bool(desc, act.flags & 1);
    g_l.follow_attach.write_bool(desc, act.flags & 2);
    g_l.ffa.write_bool(desc, act.flags & 4);
    if (!f.invoke(comp)) return fail("invoke");
    ++g_replayed;
    return true;
}

std::string describe() {
    return std::format("ability: hook={} captured={} replayed={} rejected={}\n", g_installed ? "armed" : (g_failed ? "failed" : "off"), g_captured, g_replayed, g_rejected);
}

} // namespace elmp::coop::ability
