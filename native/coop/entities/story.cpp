// Adapter story: triggers (conversations, dialogs, events, tutorials, fake walls — data/gates.json classes).
// The client never fires a trigger itself (its P1 ignores every trigger in presentation mode, copies are
// IsPlayer=false); a trigger the host has consumed is presented through the vanilla "already cleared" entry
// ATrigger::OnClearedStatusChecked(true) (fake wall gone, conversation no longer offered).

#include "coop/entities/clearable.h"

#include "engine/reflect.h"

namespace elmp::coop::entities {

namespace {

class Story final : public LevelObjects {
public:
    Story() : LevelObjects({registry::Kind::Trigger}) {}
    const char* name() const override { return "story"; }

protected:
    std::uint8_t present(ue::UObject* a, std::uint8_t flags, std::uint8_t done) override {
        static reflect::FuncRef status = reflect::Resolver("Trigger", reflect::find_class(L"/Script/Zenith.Trigger")).func(L"OnClearedStatusChecked", 1);
        if ((flags & 1) && !(done & 1) && status.ok()) {
            reflect::Frame f(status);
            f.arg_bool(0, true);
            f.invoke(a);
        }
        if ((flags & 2) && !(done & 2)) hide_actor(a);
        return flags;
    }
};

} // namespace

IEntityAdapter& story() {
    static Story a;
    return a;
}

} // namespace elmp::coop::entities
