#include "coop/entities/companion.h"
#include "engine/reflect.h"
#include <unordered_map>

namespace elmp::coop::entities {
// Only the exact original Companion.FX classifier remains. Creation, possession, leader binding,
// cooling and destruction are now owned by vanilla SpiritEquip / SpiritCompanion.
bool companion_owns_fx(ue::UObject* owner, ue::UObject* psc) {
    if (!ue::on_game_thread() || !ue::alive(owner) || !ue::alive(psc)) return false;
    static ObjHandle base;
    if (!base.valid()) base = ObjHandle::of(reflect::find_class(L"/Script/Zenith.ZenithCompanionCharacter"));
    auto* cls = static_cast<ue::UClass*>(base.get());
    if (!cls || !owner->IsA(cls)) return false;
    static std::unordered_map<ObjHandle, reflect::PropRef> properties;
    std::erase_if(properties, [](const auto& p) { return !p.first.valid(); });
    const auto type = ObjHandle::of(owner->GetClassPrivate());
    auto it = properties.find(type);
    if (it == properties.end()) {
        reflect::PropRef prop;
        if (auto* field = reflect::find_property(owner->GetClassPrivate(), L"FX"); field && reflect::kind_of(field) == reflect::Kind::Object) {
            prop.offset = field->GetOffset_Internal(); prop.kind = reflect::Kind::Object;
        }
        it = properties.emplace(type, prop).first;
    }
    return it->second.ok() && it->second.object(owner) == psc;
}
}
