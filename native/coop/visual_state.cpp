#include "coop/visual_state.h"

#include "core/schema.h"
#include "engine/handle.h"
#include "engine/reflect.h"
#include "base/clock.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace elmp::coop::visual_state {
namespace {
struct Components {
    ObjHandle actor, pivot, renderer, color;
    bool color_pinned = false;
    bool dissolve_pinned = false;
    float dissolve = 0;
};
// Handle serials prevent pointer reuse. Rebuilt on component loss, explicit forget (skin), map/session
// reset; dead actors are pruned during capture/application. describe reports cache and failure counts.
std::unordered_map<ObjHandle, Components> g_components;
reflect::PropRef g_location, g_rotation, g_scale, g_color;
reflect::FuncRef g_set_location, g_set_rotation, g_set_scale, g_set_color;
ObjHandle g_renderer_class, g_color_class;
std::uint64_t g_captured = 0, g_applied = 0, g_failures = 0, g_writes = 0;
struct DissolveSchema {
    std::array<reflect::PropRef, 4> materials;
    reflect::PropRef scalars, info, value, name, association, index;
    reflect::FuncRef set;
    ObjHandle renderer_class, mid_class, value_struct, info_struct;
    ue::FName parameter;
    std::int32_t stride = 0;
    std::int64_t attempted = 0;
    bool ready = false;
} g_dissolve;
std::uint64_t g_dissolve_captured = 0, g_dissolve_missing = 0, g_dissolve_disagree = 0, g_dissolve_invalid = 0, g_dissolve_schema_failed = 0, g_dissolve_writes = 0;

bool resolve_dissolve() {
    if (g_dissolve.ready && g_dissolve.renderer_class.valid() && g_dissolve.mid_class.valid() &&
        g_dissolve.value_struct.valid() && g_dissolve.info_struct.valid() && g_dissolve.set.handle.valid()) return true;
    const auto now = clock::now_us();
    if (g_dissolve.attempted && now - g_dissolve.attempted < 1'000'000) return false;
    g_dissolve.attempted = now;
    g_dissolve.ready = false;
    auto* renderer = reflect::find_class(L"/Script/SpinePlugin.SpineSkeletonRendererComponent");
    auto* mid = reflect::find_class(L"/Script/Engine.MaterialInstanceDynamic");
    reflect::Resolver r("Dissolve renderer", renderer), m("Dissolve material", mid);
    const wchar_t* names[] = {L"atlasNormalBlendMaterials", L"atlasAdditiveBlendMaterials", L"atlasMultiplyBlendMaterials", L"atlasScreenBlendMaterials"};
    bool arrays = true;
    for (std::size_t i = 0; i < g_dissolve.materials.size(); ++i) {
        auto& p = g_dissolve.materials[i];
        p = r.prop(names[i], reflect::Kind::Array, sizeof(reflect::RawArray));
        auto* inner = p.prop ? static_cast<RC::Unreal::FArrayProperty*>(p.prop)->GetInner() : nullptr;
        arrays &= inner && reflect::kind_of(inner) == reflect::Kind::Object && inner->GetSize() == sizeof(void*);
    }
    g_dissolve.set = r.func(L"SetScalarParameterValue", 2);
    g_dissolve.scalars = m.prop(L"ScalarParameterValues", reflect::Kind::Array, sizeof(reflect::RawArray));
    auto* inner = g_dissolve.scalars.prop ? static_cast<RC::Unreal::FArrayProperty*>(g_dissolve.scalars.prop)->GetInner() : nullptr;
    if (!r.ok() || !m.ok() || !arrays || !inner || reflect::kind_of(inner) != reflect::Kind::Struct) return ++g_dissolve_schema_failed, false;
    auto* st = static_cast<ue::UStruct*>(static_cast<RC::Unreal::FStructProperty*>(inner)->GetStruct().Get());
    reflect::Resolver v("ScalarParameterValue", st);
    g_dissolve.info = v.prop(L"ParameterInfo", reflect::Kind::Struct);
    g_dissolve.value = v.prop(L"ParameterValue", reflect::Kind::Float, 4);
    if (!v.ok()) return ++g_dissolve_schema_failed, false;
    auto* info = static_cast<ue::UStruct*>(static_cast<RC::Unreal::FStructProperty*>(g_dissolve.info.prop)->GetStruct().Get());
    reflect::Resolver p("MaterialParameterInfo", info);
    g_dissolve.name = p.prop(L"Name", reflect::Kind::Name, sizeof(ue::FName));
    g_dissolve.association = p.prop(L"Association", reflect::Kind::Byte, 1);
    g_dissolve.index = p.prop(L"Index", reflect::Kind::Int, 4);
    g_dissolve.stride = inner->GetSize();
    if (!p.ok() || g_dissolve.stride <= 0 || g_dissolve.stride > 256 ||
        g_dissolve.info.offset + g_dissolve.info.size > g_dissolve.stride || g_dissolve.value.offset + 4 > g_dissolve.stride ||
        g_dissolve.name.offset + sizeof(ue::FName) > static_cast<std::size_t>(g_dissolve.info.size) ||
        g_dissolve.association.offset + 1 > g_dissolve.info.size || g_dissolve.index.offset + 4 > g_dissolve.info.size) return ++g_dissolve_schema_failed, false;
    g_dissolve.renderer_class = ObjHandle::of(renderer); g_dissolve.mid_class = ObjHandle::of(mid);
    g_dissolve.value_struct = ObjHandle::of(st); g_dissolve.info_struct = ObjHandle::of(info);
    g_dissolve.parameter = ue::FName(L"DissolveValue", RC::Unreal::FNAME_Add);
    g_dissolve.ready = true;
    return true;
}

enum class DissolveRead { Found, Missing, Different, Invalid };
bool bounded_array(const reflect::RawArray& a, std::int32_t limit) {
    return a.num >= 0 && a.max >= a.num && a.num <= limit && a.max <= 65536 && (!a.num || a.data);
}
DissolveRead read_dissolve(ue::UObject* renderer, float& out) {
    bool any = false;
    auto* mid_cls = static_cast<ue::UClass*>(g_dissolve.mid_class.get());
    if (!renderer || !mid_cls) return DissolveRead::Missing;
    for (const auto& p : g_dissolve.materials) {
        const auto arr = p.read<reflect::RawArray>(renderer);
        if (!bounded_array(arr, 256)) return DissolveRead::Invalid;
        const auto* mids = reinterpret_cast<ue::UObject* const*>(arr.data);
        for (int i = 0; i < arr.num; ++i) {
            auto* mid = mids[i];
            if (!ue::alive(mid)) continue;
            if (!mid->GetClassPrivate()->IsChildOf(mid_cls)) return DissolveRead::Invalid;
            const auto scalars = g_dissolve.scalars.read<reflect::RawArray>(mid);
            if (!bounded_array(scalars, 1024)) return DissolveRead::Invalid;
            bool found = false;
            for (int n = 0; n < scalars.num; ++n) {
                const auto* item = scalars.data + n * g_dissolve.stride;
                const auto* info = item + g_dissolve.info.offset;
                // FName-only setter uses GlobalParameter (2) / INDEX_NONE (-1), not layer overrides.
                if (g_dissolve.name.read<ue::FName>(info) != g_dissolve.parameter ||
                    g_dissolve.association.read<std::uint8_t>(info) != 2 || g_dissolve.index.read<std::int32_t>(info) != -1) continue;
                const float value = g_dissolve.value.read<float>(item);
                if (!std::isfinite(value)) return DissolveRead::Invalid;
                if (any && std::fabs(value - out) > 0.0001f) return DissolveRead::Different;
                out = value; any = found = true;
            }
            if (!found) return DissolveRead::Missing;
        }
    }
    return any ? DissolveRead::Found : DissolveRead::Missing;
}

bool resolve() {
    if (g_location.ok() && g_rotation.ok() && g_scale.ok() && g_color.ok() &&
        g_set_location.handle.valid() && g_set_rotation.handle.valid() && g_set_scale.handle.valid() &&
        g_set_color.handle.valid() && g_renderer_class.valid() && g_color_class.valid()) return true;
    reflect::Resolver s("visual SceneComponent", reflect::find_class(L"/Script/Engine.SceneComponent"));
    g_location = s.prop(L"RelativeLocation", reflect::Kind::Struct, 12);
    g_rotation = s.prop(L"RelativeRotation", reflect::Kind::Struct, 12);
    g_scale = s.prop(L"RelativeScale3D", reflect::Kind::Struct, 12);
    g_set_location = s.func(L"K2_SetRelativeLocation", 4);
    g_set_rotation = s.func(L"K2_SetRelativeRotation", 4);
    g_set_scale = s.func(L"SetRelativeScale3D", 1);
    auto* renderer = reflect::find_class(L"/Script/SpinePlugin.SpineSkeletonRendererComponent");
    g_renderer_class = ObjHandle::of(renderer);
    g_color = reflect::Resolver("Spine renderer", renderer).prop(L"Color", reflect::Kind::Struct, 16);
    auto* color = reflect::find_class(L"/Script/Zenith.ColorComponent");
    g_color_class = ObjHandle::of(color);
    g_set_color = reflect::Resolver("ColorComponent", color).func(L"SetColor", 2);
    return s.ok() && g_color.ok() && g_set_color.ok();
}

ue::UObject* component(ue::UObject* actor, ue::UObject* cls) {
    const auto* s = core::schema::script();
    if (!s || !cls) return nullptr;
    reflect::Frame f(s->actor_component_by_class);
    f.arg(0, cls);
    return f.invoke(actor) ? f.ret<ue::UObject*>() : nullptr;
}

Components& components(ue::UObject* actor) {
    const auto h = ObjHandle::of(actor);
    if (g_components.size() > 512)
        std::erase_if(g_components, [](const auto& p) { return !p.first.get(); });
    auto& c = g_components[h];
    if (c.actor != h || !c.renderer.get() || (!c.pivot.empty() && !c.pivot.get()) || (!c.color.empty() && !c.color.get())) {
        c = {};
        c.actor = h;
        c.renderer = ObjHandle::of(component(actor, g_renderer_class.get()));
        c.color = ObjHandle::of(component(actor, g_color_class.get()));
        // Blueprint member lookup is on the known actor class, never on an arbitrary object layout.
        if (auto* p = reflect::find_property(actor->GetClassPrivate(), L"VisualPivot"); p && reflect::kind_of(p) == reflect::Kind::Object) {
            reflect::PropRef prop;
            prop.offset = p->GetOffset_Internal();
            c.pivot = ObjHandle::of(prop.object(actor));
        }
    }
    return c;
}

bool read_transform(ue::UObject* c, std::array<float, 9>& out) {
    if (!c) return false;
    std::memcpy(out.data(), g_location.at<float>(c), 12);
    std::memcpy(out.data() + 3, g_rotation.at<float>(c), 12);
    std::memcpy(out.data() + 6, g_scale.at<float>(c), 12);
    return std::all_of(out.begin(), out.end(), [](float v) { return std::isfinite(v); });
}

bool write_transform(ue::UObject* c, const std::array<float, 9>& in) {
    if (!c) return false;
    bool ok = true;
    const reflect::PropRef* props[] = {&g_location, &g_rotation, &g_scale};
    const reflect::FuncRef* funcs[] = {&g_set_location, &g_set_rotation, &g_set_scale};
    for (int i = 0; i < 3; ++i) {
        if (std::memcmp(props[i]->at<float>(c), in.data() + i * 3, 12) == 0) continue;
        reflect::Frame f(*funcs[i]);
        std::memcpy(f.raw(0), in.data() + i * 3, 12);
        if (i != 2) f.arg_bool(1, false).arg_bool(3, true);
        ok &= f.invoke(c);
        ++g_writes;
    }
    return ok;
}
} // namespace

void capture(ue::UObject* actor, net::snap::Visual& out) {
    out = {};
    if (!ue::alive(actor) || !resolve()) return void(++g_failures);
    auto& c = components(actor);
    if (read_transform(c.pivot.get(), out.pivot)) out.valid |= 1;
    if (read_transform(c.renderer.get(), out.renderer)) out.valid |= 2;
    if (auto* r = c.renderer.get()) {
        std::memcpy(out.color.data(), g_color.at<float>(r), 16);
        if (std::all_of(out.color.begin(), out.color.end(), [](float v) { return std::isfinite(v); })) out.valid |= 4;
        if (resolve_dissolve()) {
            switch (read_dissolve(r, out.dissolve)) {
            case DissolveRead::Found: out.valid |= 8; ++g_dissolve_captured; break;
            case DissolveRead::Missing: out.dissolve = 0; ++g_dissolve_missing; break;
            case DissolveRead::Different: out.dissolve = 0; ++g_dissolve_disagree; break;
            case DissolveRead::Invalid: out.dissolve = 0; ++g_dissolve_invalid; break;
            }
        }
    }
    ++g_captured;
}

bool apply(ue::UObject* actor, const net::snap::Visual& in) {
    if (!ue::alive(actor) || !resolve()) return ++g_failures, false;
    auto& c = components(actor);
    bool ok = true;
    if (in.valid & 1) ok &= write_transform(c.pivot.get(), in.pivot);
    if (in.valid & 2) ok &= write_transform(c.renderer.get(), in.renderer);
    if (in.valid & 4) {
        auto* r = c.renderer.get();
        if (!r) ok = false;
        else if (!c.color_pinned || std::memcmp(g_color.at<float>(r), in.color.data(), 16) != 0) {
            if (auto* color = c.color.get()) {
                reflect::Frame f(g_set_color);
                std::memcpy(f.raw(0), in.color.data(), 16);
                f.arg(1, 0.0f); // original entry writes renderer and ends local interpolation (IDA CBC380).
                ok &= f.invoke(color);
            }
            std::memcpy(g_color.at<float>(r), in.color.data(), 16);
            c.color_pinned = true;
            ++g_writes;
        }
    }
    if (in.valid & 8) {
        auto* r = c.renderer.get();
        if (!r || !std::isfinite(in.dissolve) || !resolve_dissolve()) ok = false;
        else if (!c.dissolve_pinned || c.dissolve != in.dissolve) {
            // Shipping 7ED110 caches the named scalar and 7E3350 applies it to all four blend groups.
            // This is renderer output only: never call DeathStart, a death delegate, or a Timeline.
            reflect::Frame f(g_dissolve.set);
            f.arg_name(0, g_dissolve.parameter).arg(1, in.dissolve);
            if (f.invoke(r)) { c.dissolve_pinned = true; c.dissolve = in.dissolve; ++g_dissolve_writes; ++g_writes; }
            else ok = false;
        }
    }
    ++g_applied;
    if (!ok) ++g_failures;
    return ok;
}

void forget(ue::UObject* actor) { g_components.erase(ObjHandle::of(actor)); }
void reset() { g_components.clear(); }
std::string describe() {
    return std::format("visual: cached={} capture={} apply={} writes={} failures={}\n", g_components.size(), g_captured, g_applied, g_writes, g_failures) +
           std::format("dissolve: ready={} captured={} writes={} missing={} disagree={} invalid={} schemaFailed={}\n", g_dissolve.ready, g_dissolve_captured,
                       g_dissolve_writes, g_dissolve_missing, g_dissolve_disagree, g_dissolve_invalid, g_dissolve_schema_failed);
}
} // namespace elmp::coop::visual_state
