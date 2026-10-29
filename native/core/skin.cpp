#include "core/skin.h"

#include "core/schema.h"

#include "engine/reflect.h"

#include "base/log.h"

#include <cmath>
#include <cstring>
#include <format>

namespace elmp::core::skin {

namespace {

std::wstring base_name(int n) { return std::format(L"p{:04}_Lily", n); }

constexpr const wchar_t* kNotify = L"/Game/_Zenith/Characters/p0001_Lily/p0001_Lily-notify.p0001_Lily-notify";

} // namespace

std::string apply(ue::UObject* pawn, int n) {
    if (!valid(n)) return std::format("skin must be 1..{}", kCount);
    const auto* s = schema::script();
    const auto* bp = schema::blueprint();
    if (!s || !bp) return "core schema unresolved";
    auto* spine = pawn ? bp->lily_spine.object(pawn) : nullptr;
    if (!spine) return "pawn has no Spine component";
    const auto name = base_name(n);
    const auto dir = std::format(L"/Game/_Zenith/Characters/{0}/{0}", name);
    auto* atlas = reflect::load_object(std::format(L"{}.{}-atlas", dir, name));
    auto* data = reflect::load_object(std::format(L"{}.{}-data", dir, name));
    auto* notify = reflect::load_object(kNotify);
    if (!atlas || !data || !notify) return std::format("skin {} assets not found", n);

    reflect::Frame replace(s->sp_replace);
    replace.arg(0, atlas).arg(1, data).arg(2, notify);
    if (!replace.invoke(spine)) return "ReplaceSpineData failed";
    reflect::Frame(s->sp_setup_pose).invoke(spine);
    reflect::Frame(s->sp_slots_setup).invoke(spine);
    reflect::Frame(s->sp_update_world).invoke(spine);
    if (s->sp_force_tick.ok()) reflect::Frame(s->sp_force_tick).arg(0, 0.016f).invoke(spine);
    reflect::Frame anim(s->sp_set_animation);
    anim.arg(0, 0).arg_string(1, L"idle").arg_bool(2, true);
    anim.invoke(spine);

    // Vanilla BP_p0000_Lily.SetSpineData also sets the FakeLight material (optional component).
    if (auto* light = bp->lily_fake_light.ok() ? bp->lily_fake_light.object(pawn) : nullptr) {
        if (auto* mi = reflect::load_object(std::format(L"/Game/_Zenith/Characters/p0000_Lily/MI_LilyLight{0}.MI_LilyLight{0}", n)))
            reflect::Frame(s->prim_set_material).arg(0, 0).arg(1, mi).invoke(light);
    }
    return {};
}

namespace {

ue::UObject* component_of(ue::UObject* actor, const wchar_t* cls_path) {
    const auto* s = schema::script();
    if (!s || !actor || !s->actor_component_by_class.ok()) return nullptr;
    auto* cls = reflect::find_class(cls_path);
    if (!cls) return nullptr;
    reflect::Frame f(s->actor_component_by_class);
    f.arg(0, static_cast<ue::UObject*>(cls));
    return f.invoke(actor) ? f.ret<ue::UObject*>() : nullptr;
}

// <pkg>.<Class> → <pkg>.Default__<Class>: the class defaults carry the BP component templates.
ue::UObject* cdo_of(ue::UObject* a) {
    auto* cls = a ? a->GetClassPrivate() : nullptr;
    if (!cls) return nullptr;
    std::wstring path = cls->GetPathName();
    const auto dot = path.rfind(L'.');
    if (dot == std::wstring::npos) return nullptr;
    return reflect::find_object(path.substr(0, dot + 1) + L"Default__" + path.substr(dot + 1));
}

} // namespace

void normalize(ue::UObject* pawn) {
    const auto* s = schema::script();
    if (!s || !pawn) return;
    // 1) 渲染组件的相对变换：蓝图模板上 Lily 的 SpineSkeletonRenderer 是 -65（脚底对齐），
    //    我们复制出来的 Pawn 有时是 0 → 整个人高半身。抄 CDO 的值。
    if (auto* mine = component_of(pawn, L"/Script/SpinePlugin.SpineSkeletonRendererComponent")) {
        static reflect::FuncRef set_rel;
        if (!set_rel.ok()) {
            static reflect::Resolver r("SceneComponent", reflect::find_class(L"/Script/Engine.SceneComponent"));
            set_rel = r.func(L"K2_SetRelativeLocation", 3);
            if (!set_rel.ok()) set_rel = r.func(L"SetRelativeLocation", 4);
        }
        if (auto* def = cdo_of(pawn))
            if (auto* theirs = component_of(def, L"/Script/SpinePlugin.SpineSkeletonRendererComponent"))
                if (s->scene_rel_location.ok()) {
                    const float* v = s->scene_rel_location.at<float>(theirs);
                    float xyz[3];
                    std::memcpy(xyz, v, sizeof(xyz));
                    auto* dst = s->scene_rel_location.at<float>(mine);
                    if (std::memcmp(dst, xyz, sizeof(xyz)) != 0) {
                        std::memcpy(dst, xyz, sizeof(xyz));
                        if (set_rel.ok()) {
                            reflect::Frame f(set_rel);
                            if (auto* arg = static_cast<float*>(f.raw(0))) std::memcpy(arg, xyz, sizeof(xyz));
                            f.arg_bool(1, false).arg_bool(2, false);
                            f.invoke(mine);
                        }
                        log::info("coop: renderer offset of {} restored to ({:.0f},{:.0f},{:.0f})", log::utf8(ue::name_of(pawn)), xyz[0], xyz[1], xyz[2]);
                    }
                }
    }
    // 2) 颜色组件的默认颜色：复制出来的 Pawn 常常是 (0,0,0,1) → 全黑剪影；原版是白。
    //    ClearColor(lerp) 的语义就是“回到默认色”（实测调用后读回 (1,1,1,1)）。
    static ObjHandle color_cls = ObjHandle::of(reflect::find_object(L"/Script/Zenith.ColorComponent"));
    static reflect::FuncRef clear_color;
    if (!clear_color.ok())
        clear_color = reflect::Resolver("ColorComponent", reflect::find_class(L"/Script/Zenith.ColorComponent")).func(L"ClearColor", 1);
    if (clear_color.ok())
        if (auto* cc = color_cls.get() ? component_of(pawn, L"/Script/Zenith.ColorComponent") : nullptr) {
            static reflect::FuncRef get_color = reflect::Resolver("ColorComponent", reflect::find_class(L"/Script/Zenith.ColorComponent")).func(L"GetCurrentColor", 0);
            bool black = true;
            if (get_color.ok()) {
                reflect::Frame g(get_color);
                if (g.invoke(cc))
                    if (const auto* c = static_cast<const float*>(g.raw(get_color.ret))) black = c[0] + c[1] + c[2] < 0.01f;
            }
            if (black) {
                reflect::Frame f(clear_color);
                f.arg(0, 0.0f);
                if (f.invoke(cc)) log::info("coop: colour of {} reset to the default (was black)", log::utf8(ue::name_of(pawn)));
            }
        }
}

float renderer_z(ue::UObject* actor) {
    const auto* s = schema::script();
    auto* r = component_of(actor, L"/Script/SpinePlugin.SpineSkeletonRendererComponent");
    if (!s || !r || !s->scene_rel_location.ok()) return 0.0f;
    return s->scene_rel_location.at<float>(r)[2];
}

bool set_renderer_z(ue::UObject* actor, float z) {
    return set_renderer_z_comp(component_of(actor, L"/Script/SpinePlugin.SpineSkeletonRendererComponent"), z);
}

bool set_renderer_z_comp(ue::UObject* r, float z) {
    const auto* s = schema::script();
    if (!s || !r || !s->scene_rel_location.ok()) return false;
    float* v = s->scene_rel_location.at<float>(r);
    if (std::fabs(v[2] - z) < 0.5f) return false;
    v[2] = z;
    static reflect::FuncRef set_rel;
    if (!set_rel.ok()) set_rel = reflect::Resolver("SceneComponent", reflect::find_class(L"/Script/Engine.SceneComponent")).func(L"K2_SetRelativeLocation", 3);
    if (set_rel.ok()) {
        reflect::Frame f(set_rel);
        if (auto* p = static_cast<float*>(f.raw(0))) p[0] = v[0], p[1] = v[1], p[2] = v[2];
        f.arg_bool(1, false).arg_bool(2, false);
        f.invoke(r);
    }
    return true;
}

int current(ue::UObject* pawn) {    const auto* s = schema::script();
    const auto* bp = schema::blueprint();
    auto* spine = (s && bp && pawn) ? bp->lily_spine.object(pawn) : nullptr;
    auto* atlas = spine ? s->sk_atlas.object(spine) : nullptr;
    if (!atlas) return 0;
    const auto name = ue::name_of(atlas); // "p000N_Lily-atlas"
    if (name.size() < 5 || name[0] != L'p') return 0;
    const int n = std::wcstol(name.c_str() + 1, nullptr, 10);
    return valid(n) ? n : 0;
}

} // namespace elmp::core::skin
