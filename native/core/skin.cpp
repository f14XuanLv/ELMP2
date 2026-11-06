#include "core/skin.h"

#include "core/schema.h"

#include "engine/reflect.h"

#include "base/log.h"
#include <Unreal/Engine/UDataTable.hpp>

#include <cmath>
#include <cstring>
#include <format>

namespace elmp::core::skin {

std::string apply(ue::UObject* pawn, int n) {
    if (!valid(n)) return std::format("skin must be 1..{}", kCount);
    const auto* s = schema::script();
    const auto* bp = schema::blueprint();
    if (!s || !bp) return "core schema unresolved";
    if (!pawn) return "pawn missing";
    reflect::Resolver r("Lily SetSpineData", pawn->GetClassPrivate());
    const auto table_prop = r.prop(L"SpineDataDatatable", reflect::Kind::Object);
    const auto set = r.func(L"SetSpineData", 1);
    if (!r.ok()) return r.failure();
    auto* table = static_cast<RC::Unreal::UDataTable*>(table_prop.object(pawn));
    if (!table || !table->GetRowStruct() || set.parms[0].kind != reflect::Kind::Struct ||
        set.parms[0].size != reflect::properties_size(table->GetRowStruct())) return "skin DataTable/parameter struct mismatch";
    constexpr int levels[] = {1, 2, 5, 7, 9, 11}; // first table level of each of the six vanilla looks
    const auto row_name = std::to_string(levels[n - 1]);
    for (const auto& row : table->GetRowMap()) {
        if (ue::utf8(row.Key) != row_name) continue;
        reflect::Frame call(set);
        set.parms[0].prop->CopyCompleteValue(call.raw(0), row.Value);
        return call.invoke(pawn) ? std::string{} : "original SetSpineData failed";
    }
    return std::format("skin table has no row {}", n);
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
