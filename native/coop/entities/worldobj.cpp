// Adapter worldobj: interactables (doors / chests / items / switches) and breakables.
// Client presentation: host cleared → the vanilla "already cleared" entry OnClearedStatusChecked(true) (the
// call the map makes for objects cleared in the save: opened door / chest, consumed item); host gone or
// breakable broken → hidden + no collision. The client never replays the vanilla break (drops would spawn
// locally); nothing is marked cleared in the client's save (and saving is refused by the guard anyway).

#include "coop/entities/clearable.h"

#include "core/schema.h"

#include "engine/reflect.h"

namespace elmp::coop::entities {

namespace {

class WorldObj final : public LevelObjects {
public:
    WorldObj() : LevelObjects({registry::Kind::Interactable, registry::Kind::Breakable}) {}
    const char* name() const override { return "worldobj"; }

protected:
    std::uint8_t host_extra(registry::Kind kind, ue::UObject* a) override {
        if (kind != registry::Kind::Breakable) return 0;
        // ABreakable has its own HPComponent (the ZenithCharacter offset on a breakable crashed: AV 0x10000000b)
        static reflect::PropRef hp_prop =
            reflect::Resolver("Breakable", reflect::find_class(L"/Script/Zenith.Breakable")).prop(L"HPComponent", reflect::Kind::Object);
        const auto* s = core::schema::script();
        auto* hp = hp_prop.ok() ? hp_prop.object(a) : nullptr;
        return hp && s && s->hp_curr.read<std::int32_t>(hp) <= 0 ? 4 : 0; // broken (IsCleared alone ≠ broken)
    }

    std::uint8_t present(ue::UObject* a, std::uint8_t flags, std::uint8_t done) override {
        static ue::UClass* interactable = reflect::find_class(L"/Script/Zenith.Interactable");
        static ue::UClass* breakable = reflect::find_class(L"/Script/Zenith.Breakable");
        static reflect::FuncRef status =
            reflect::Resolver("Interactable", reflect::find_class(L"/Script/Zenith.Interactable")).func(L"OnClearedStatusChecked", 1);
        std::uint8_t out = flags;
        if ((flags & 1) && !(done & 1) && interactable && a->GetClassPrivate()->IsChildOf(interactable) && status.ok()) {
            reflect::Frame f(status);
            f.arg_bool(0, true);
            f.invoke(a);
        }
        const bool is_breakable = breakable && a->GetClassPrivate()->IsChildOf(breakable);
        if (((flags & 2) || (is_breakable && (flags & 4))) && !(done & 2)) {
            hide_actor(a);
            out |= 2;
        }
        return out;
    }
};

} // namespace

IEntityAdapter& worldobj() {
    static WorldObj a;
    return a;
}

} // namespace elmp::coop::entities
