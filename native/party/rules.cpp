#include "party/rules.h"

#include "party/config.h"
#include "party/seat.h"

#include "core/permission.h"
#include "core/player.h"
#include "core/schema.h"

#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"

#include "base/log.h"
#include "base/settings.h"

#include <cmath>
#include <format>
#include <unordered_map>

namespace elmp::party::rules {

namespace {

struct Box {
    std::int32_t serial = 0;
    core::Vec3 center, extent;
    bool ok = false;
};
std::unordered_map<std::int32_t, Box> g_boxes;
std::int32_t g_open = 0;
std::string g_note;

const Box& box_of(const registry::Entry& e) {
    auto& b = g_boxes[e.handle.index];
    if (b.serial == e.handle.serial) return b;
    b = Box{e.handle.serial};
    const auto* s = core::schema::script();
    auto* door = e.handle.get();
    auto* comp = s && door ? s->trigger_collision.object(door) : nullptr;
    if (!comp) return b;
    reflect::Frame f(s->actor_location);
    if (!f.invoke(door)) return b;
    b.center = f.ret<core::Vec3>();
    const auto ext = s->box_extent.read<core::Vec3>(comp);
    const auto scale = s->scene_rel_scale.read<core::Vec3>(comp);
    b.extent = {std::abs(ext.x * scale.x), std::abs(ext.y * scale.y), std::abs(ext.z * scale.z)};
    b.ok = true;
    return b;
}

bool inside(const Box& b, const core::Vec3& p) {
    return b.ok && std::abs(p.y - b.center.y) <= b.extent.y && std::abs(p.z - b.center.z) <= b.extent.z; // game plane is Y/Z
}

bool grant(std::int64_t slot, core::permission::Gate g) {
    // rules 1 / 2 are for local seats only (§7.4); a remote (network) seat keeps its own travel gate (C18)
    const auto* s = party::seat(slot);
    return g == core::permission::Gate::Travel && config::map_rule() == 1 && slot >= 2 && slot <= 100 && s && s->source != party::Source::Network;
}

} // namespace

void tick() {
    if (config::map_rule() != 2 || party::count() == 0) {
        core::permission::hold_p1_doors(false);
        g_open = 0;
        return;
    }
    const auto seats = party::seats();
    std::int32_t open = 0;
    for (const auto& e : registry::bucket(registry::Kind::TravelVolume)) {
        const auto& b = box_of(e);
        if (!b.ok) continue;
        bool all = true;
        for (const auto& seat : seats) {
            auto* p = core::pawn(seat.slot);
            const auto pos = p ? core::location(p) : std::nullopt;
            if (!pos || !inside(b, *pos)) {
                all = false;
                break;
            }
        }
        if (all) {
            open = e.handle.index;
            break;
        }
    }
    if (open != g_open) {
        g_note = open ? "all players at a door; P1 can use it" : "";
        if (open) log::info("map rule 2: every local player is at a door; P1 may travel");
    }
    g_open = open;
    core::permission::hold_p1_doors(true, open);
}

std::string describe() {
    return std::format("mapRule={} doors={} open={} {}\n", config::map_rule(), registry::size(registry::Kind::TravelVolume), g_open, g_note);
}

void install() {
    core::permission::set_grant(&grant);
    settings::on_change([](const std::string& key) {
        if (key == "mapRule") core::permission::set_grant(&grant); // re-evaluate every seat's travel gate
    });
    lifecycle::on_teardown([] {
        g_boxes.clear();
        g_open = 0;
    });
}

} // namespace elmp::party::rules
