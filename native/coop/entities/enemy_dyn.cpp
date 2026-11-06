// Adapter enemy_dyn: enemies without a SourceSpawnPoint (summons, boss reinforcements, scripted spawns).
// Host rows (section ed): key = host-assigned dyn id per object handle, type = class path name id, position /
// facing / HP / dead flag (pose as section e). Client (coop/client.cpp present_dynamic): adopt an unbound
// local enemy of the same class within 300 UU (the local pipeline may have produced the same summon), else a
// presentation copy (deferred spawn); then the enemy takeover recipe (freeze, AI gate, IsPlayer=false) and
// the same row presentation as spawn-point enemies. Row gone → adopted object hidden / copy destroyed.

#include "coop/client.h"
#include "coop/host.h"
#include "coop/presentation.h"
#include "coop/visual_state.h"
#include "coop/entity.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/handle.h"
#include "engine/reflect.h"
#include "engine/registry.h"

#include "base/log.h"

#include <format>
#include <unordered_map>

namespace elmp::coop::entities {

namespace {

class EnemyDyn final : public IEntityAdapter {
public:
    const char* name() const override { return "enemy_dyn"; }

    void write_host(HostCtx& c) override {
        const auto* s = core::schema::script();
        static reflect::PropRef spawn_point =
            reflect::Resolver("ZenithCharacter", reflect::find_class(L"/Script/Zenith.ZenithCharacter")).prop(L"SourceSpawnPoint", reflect::Kind::Object);
        if (!s || !spawn_point.ok()) return;
        for (const auto& e : registry::bucket(registry::Kind::Enemy)) {
            auto* a = e.handle.get();
            if (!a || spawn_point.object(a)) continue; // spawn-point enemies: section e
            const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(e.handle.index)) << 32) | static_cast<std::uint32_t>(e.handle.serial);
            auto [it, fresh] = ids_.try_emplace(key, 0);
            if (fresh) it->second = next_id_ = static_cast<std::uint16_t>(next_id_ % 60000 + 1);
            auto* row = c.frame.ed.add(it->second);
            if (!row) break;
            row->flags = host::enemy_flags(a);
            if (auto at = core::location(a)) row->x = at->x, row->y = at->y, row->z = at->z;
            if (auto* hp = s->zc_hp.object(a)) {
                const auto cur = s->hp_curr.read<std::int32_t>(hp);
                row->hp = static_cast<std::uint16_t>(std::max(0, cur));
                row->mhp = static_cast<std::uint16_t>(std::max(0, s->hp_max.read<std::int32_t>(hp)));
                if (cur <= 0) row->flags |= 4;
            }
            if (auto* loco = s->zc_locomotion.object(a)) {
                reflect::Frame fr(s->loco_facing_right);
                if (fr.invoke(loco)) row->facing = fr.ret_bool() ? 1 : -1;
            }
            auto& path = paths_[e.class_id];
            if (path.empty()) path = log::utf8(a->GetClassPrivate()->GetPathName());
            row->type = c.name(path);
            presentation::capture(a, c.name, row->pose);
            visual_state::capture(a, row->visual);
        }
    }

    void apply_client(const ClientCtx& c) override { client::present_dynamic(c); }
    void reset() override { ids_.clear(); }
    std::uint16_t id_of(const ObjHandle& h) const {
        const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(h.index)) << 32) | static_cast<std::uint32_t>(h.serial);
        auto it = ids_.find(key);
        return it == ids_.end() ? 0 : it->second;
    }
    std::string describe() const override { return std::format("enemy_dyn: hostIds={}\n", ids_.size()) + client::describe_dynamic(); }

private:
    std::unordered_map<std::uint64_t, std::uint16_t> ids_;
    std::unordered_map<std::uint16_t, std::string> paths_;
    std::uint16_t next_id_ = 0;
};

} // namespace

EnemyDyn& dyn_singleton() {
    static EnemyDyn a;
    return a;
}

IEntityAdapter& enemy_dyn() { return dyn_singleton(); }

std::uint16_t dyn_id_of(const ObjHandle& h) { return dyn_singleton().id_of(h); }

} // namespace elmp::coop::entities

namespace elmp::coop {

std::span<IEntityAdapter* const> adapters() {
    static IEntityAdapter* const list[] = {&entities::lily(), &entities::enemy(), &entities::worldobj(), &entities::story(), &entities::pickup(), &entities::enemy_dyn()};
    return list;
}

} // namespace elmp::coop
