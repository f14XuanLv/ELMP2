// Adapter pickup: drops on the host (registry Collectable bucket; pooled AReusableActor, visible = active).
// Host rows: key = host-assigned dyn id (per object handle; actor names of pooled drops are not stable
// across processes), position, class path. Client: a presentation copy of the same class at the host
// position — actor tick and collision off (it must not home in on / be collected by the local lily; the
// local P1 collect sphere is off in presentation mode anyway). Row gone → copy destroyed. Collection is
// settled by the host (the guest's seat collects there); the client never grants an item.

#include "coop/entities/spawn.h"
#include "coop/entity.h"

#include "core/player.h"

#include "engine/handle.h"
#include "engine/reflect.h"
#include "engine/registry.h"

#include "base/log.h"

#include <format>
#include <map>
#include <unordered_map>

namespace elmp::coop::entities {

namespace {

class Pickup final : public IEntityAdapter {
public:
    const char* name() const override { return "pickup"; }

    void write_host(HostCtx& c) override {
        static reflect::PropRef hidden = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).prop(L"bHidden", reflect::Kind::Bool);
        for (const auto& e : registry::bucket(registry::Kind::Collectable)) {
            auto* a = e.handle.get();
            if (!a || (hidden.ok() && hidden.read_bool(a))) continue; // pooled + parked
            const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(e.handle.index)) << 32) | static_cast<std::uint32_t>(e.handle.serial);
            auto [it, fresh] = ids_.try_emplace(key, 0);
            if (fresh) it->second = next_id_ = static_cast<std::uint16_t>(next_id_ % 60000 + 1);
            auto* row = c.frame.pk.add(it->second);
            if (!row) break;
            if (auto at = core::location(a)) row->x = at->x, row->y = at->y, row->z = at->z;
            auto& path = paths_[e.class_id];
            if (path.empty()) path = log::utf8(a->GetClassPrivate()->GetPathName());
            row->cls = c.name(path);
        }
    }

    void apply_client(const ClientCtx& c) override {
        for (auto& [k, cp] : copies_) cp.seen = false;
        for (const auto& row : c.frame.pk.view()) {
            auto& cp = copies_[row.key];
            cp.seen = true;
            auto* a = cp.actor.get();
            if (!a) {
                const auto* path = c.text(row.cls);
                auto* cls = path ? class_by_path(*path) : nullptr;
                if (!cls) continue;
                a = spawn_copy(cls, row.x, row.y, row.z);
                if (!a) continue;
                freeze(a);
                cp.actor = ObjHandle::of(a);
                ++spawned_;
            } else if (row.x != cp.x || row.y != cp.y || row.z != cp.z) {
                move_copy(a, row.x, row.y, row.z);
            }
            cp.x = row.x, cp.y = row.y, cp.z = row.z;
        }
        for (auto it = copies_.begin(); it != copies_.end();)
            if (!it->second.seen) {
                destroy_copy(it->second.actor.get());
                ++removed_;
                it = copies_.erase(it);
            } else {
                ++it;
            }
    }

    void reset() override {
        for (auto& [k, cp] : copies_) destroy_copy(cp.actor.get());
        copies_.clear();
        ids_.clear();
    }

    std::string describe() const override {
        return std::format("pickup: hostIds={} copies={} spawned={} removed={}\n", ids_.size(), copies_.size(), spawned_, removed_);
    }

private:
    static void freeze(ue::UObject* a) {
        static reflect::Resolver r("Actor", reflect::find_class(L"/Script/Engine.Actor"));
        static reflect::FuncRef tick = r.func(L"SetActorTickEnabled", 1);
        static reflect::FuncRef coll = r.func(L"SetActorEnableCollision", 1);
        if (tick.ok()) reflect::Frame(tick).arg_bool(0, false).invoke(a);
        if (coll.ok()) reflect::Frame(coll).arg_bool(0, false).invoke(a);
    }

    struct CopyRec {
        ObjHandle actor;
        float x = 0, y = 0, z = 0;
        bool seen = false;
    };
    std::unordered_map<std::uint64_t, std::uint16_t> ids_;
    std::unordered_map<std::uint16_t, std::string> paths_;
    std::map<std::uint16_t, CopyRec> copies_;
    std::uint16_t next_id_ = 0;
    std::uint64_t spawned_ = 0, removed_ = 0;
};

} // namespace

IEntityAdapter& pickup() {
    static Pickup a;
    return a;
}

} // namespace elmp::coop::entities
