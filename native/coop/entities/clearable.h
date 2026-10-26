#pragma once

// Shared base of the level-object adapters (worldobj, story): level-placed actors keyed by actor name
// (identical in both processes), host-side state sampled at 2 Hz (ProcessEvent per object), rows only for
// objects with state (absence = untouched). Row flags: bit0 cleared (UClearableComponent::IsCleared), bit1
// gone (destroyed on the host while this map is loaded), bit2 broken (breakable HP <= 0).

#include "coop/entity.h"

#include "engine/handle.h"
#include "engine/registry.h"

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace elmp::coop::entities {

class LevelObjects : public IEntityAdapter {
public:
    explicit LevelObjects(std::initializer_list<registry::Kind> kinds) : kinds_(kinds) {}
    void write_host(HostCtx& c) override;
    void apply_client(const ClientCtx& c) override;
    void reset() override;
    std::string describe() const override;

protected:
    // client presentation of one row for a local object; returns the flags now presented
    virtual std::uint8_t present(ue::UObject* actor, std::uint8_t flags, std::uint8_t done) = 0;
    virtual std::uint8_t host_extra(registry::Kind, ue::UObject*) { return 0; }

    std::vector<registry::Kind> kinds_;

private:
    struct Obj {
        ObjHandle actor, clearable;
        std::uint8_t flags = 0;
    };
    std::unordered_map<std::string, Obj> host_;
    std::unordered_map<std::string, std::uint8_t> done_;
    std::int64_t next_sample_us_ = 0;
    std::uint64_t presented_ = 0;
};

// client helpers shared by the adapters
void hide_actor(ue::UObject* actor);

} // namespace elmp::coop::entities
