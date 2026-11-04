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
