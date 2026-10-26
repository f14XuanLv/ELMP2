#include "coop/maps.h"

#include "party/seat.h"
#include "party/transit.h"

#include "core/permission.h"
#include "core/player.h"
#include "core/schema.h"

#include "world/level_instance.h"

#include "engine/handle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/log.h"

#include <Unreal/UObjectGlobals.hpp>

#include <array>
#include <cmath>
#include <format>
#include <unordered_map>
#include <vector>

namespace elmp::coop::maps {

namespace {

constexpr int kSlots = 4; // background slots 1..3
constexpr std::int64_t kGraceUs = 10'000'000;

struct Start {
    core::Vec3 at{};
    std::string tag;
};
struct Slot {
    std::string map;
    std::int64_t empty_since = 0;
    bool starts_taken = false;
    std::vector<Start> starts; // the instance's PlayerStarts (their vanilla tags are cleared, see below)
};
std::array<Slot, kSlots> g_slots;

struct Pending { // a seat waiting for its background map
    std::int64_t seat = 0;
    int slot = 0;
    std::string tag;
};
std::vector<Pending> g_pending;
std::uint64_t g_moves = 0, g_loads = 0, g_unloads = 0;
std::string g_last;
std::unordered_map<std::int64_t, std::string> g_seat_tag;

struct Door {
    ObjHandle handle;
    core::Vec3 center{}, extent{};
    std::string dest, tag;
    bool ok = false;
};
std::unordered_map<std::int32_t, Door> g_doors;

const Door& door_of(const registry::Entry& e) {
    auto& d = g_doors[e.handle.index];
    if (d.handle == e.handle) return d;
    d = Door{e.handle};
    const auto* s = core::schema::script();
    auto* a = e.handle.get();
    auto* comp = s && a ? s->trigger_collision.object(a) : nullptr;
    if (!comp) return d;
    static reflect::Resolver r("WorldTravelVolume", static_cast<ue::UStruct*>(a->GetClassPrivate()));
    static reflect::PropRef map_prop = r.prop(L"GameMapToLoad", reflect::Kind::Struct);
    static reflect::PropRef tag_prop = r.prop(L"PlayerStartTag", reflect::Kind::Name);
    if (!map_prop.ok() || !tag_prop.ok()) return d;
    // FDataTableRowHandle { UDataTable* DataTable; FName RowName; }
    const auto* row = reinterpret_cast<const ue::FName*>(map_prop.at<std::uint8_t>(a) + 8);
    d.dest = ue::utf8(*row);
    d.tag = ue::utf8(*tag_prop.at<ue::FName>(a));
    reflect::Frame f(s->actor_location);
    if (!f.invoke(a)) return d;
    d.center = f.ret<core::Vec3>();
    const auto ext = s->box_extent.read<core::Vec3>(comp);
    const auto scale = s->scene_rel_scale.read<core::Vec3>(comp);
    d.extent = {std::abs(ext.x * scale.x), std::abs(ext.y * scale.y), std::abs(ext.z * scale.z)};
    d.ok = !d.dest.empty();
    return d;
}

std::vector<RC::Unreal::UObject*> starts_in(int slot) {
    std::vector<RC::Unreal::UObject*> all, out;
    RC::Unreal::UObjectGlobals::FindAllOf(STR("PlayerStart"), all);
    for (auto* ps : all)
        if (ue::alive(ps))
            if (auto at = core::location(ps); at && slot_of_y(at->y) == slot) out.push_back(ps);
    return out;
}

reflect::PropRef& start_tag() {
    static reflect::PropRef p =
        reflect::Resolver("PlayerStart", reflect::find_class(L"/Script/Engine.PlayerStart")).prop(L"PlayerStartTag", reflect::Kind::Name);
    return p;
}

// A background instance's PlayerStarts carry the same tags as the vanilla ones: when host P1 later switches
// into that map, the vanilla placement found the background copy first (P1 placed at Y +300000, fell when the
// copy unloaded — 2026-10-27). So once the instance is ready ELMP records them and clears their tags.
void take_starts(Slot& sl, int slot) {
    if (sl.starts_taken || !start_tag().ok()) return;
    for (auto* ps : starts_in(slot)) {
        auto* tag = start_tag().at<ue::FName>(ps);
        sl.starts.push_back({*core::location(ps), ue::utf8(*tag)});
        *tag = ue::FName();
    }
    sl.starts_taken = !sl.starts.empty(); // none yet (actors still registering): try again next poll
}

// PlayerStart of `slot` whose tag == tag (any start of the slot when the tag is not found)
std::optional<core::Vec3> player_start(int slot, const std::string& tag) {
    if (slot > 0) {
        auto& sl = g_slots[slot];
        take_starts(sl, slot);
        std::optional<core::Vec3> any;
        for (const auto& s : sl.starts) {
            if (!any) any = s.at;
            if (s.tag == tag) return s.at;
        }
        return any;
    }
    std::optional<core::Vec3> any;
    for (auto* ps : starts_in(0)) {
        const auto at = core::location(ps);
        if (!any) any = at;
        if (start_tag().ok() && ue::utf8(*start_tag().at<ue::FName>(ps)) == tag) return at;
    }
    return any;
}
bool place(std::int64_t seat, const core::Vec3& at) {
    const auto* s = core::schema::script();
    auto* pawn = core::pawn(seat);
    if (!s || !pawn) return false;
    reflect::Frame tp(s->actor_teleport);
    tp.arg(0, core::Vec3{at.x, at.y, at.z + 40.0f}).arg_bool(1, false).arg_bool(3, true);
    return tp.invoke(pawn);
}

int slot_for(const std::string& map) {
    for (int k = 1; k < kSlots; ++k)
        if (g_slots[k].map == map) return k;
    for (int k = 1; k < kSlots; ++k)
        if (g_slots[k].map.empty()) return k;
    return -1;
}

void move_seat(std::int64_t seat, const Door& d) {
    const auto fg = ue::utf8(world::current_map_id());
    if (d.dest == fg) {
        if (auto at = player_start(0, d.tag)) place(seat, *at), ++g_moves, g_last = std::format("P{} -> {} (foreground)", seat, d.dest);
        return;
    }
    const int k = slot_for(d.dest);
    if (k < 0) return void(g_last = std::format("P{} -> {}: no free background slot (fail closed)", seat, d.dest));
    if (g_slots[k].map.empty()) {
        if (auto e = world::level_instance::load(d.dest, offset(k)); !e.empty())
            return void(g_last = std::format("P{} -> {}: {} (fail closed)", seat, d.dest, e));
        g_slots[k].map = d.dest;
        ++g_loads;
    }
    for (auto& p : g_pending)
        if (p.seat == seat) return;
    g_pending.push_back({seat, k, d.tag});
    g_seat_tag[seat] = d.tag;
    g_last = std::format("P{} -> {} (background slot {}, loading)", seat, d.dest, k);
}

} // namespace

int slot_of_y(float y) {
    const int k = static_cast<int>(std::lround(y / kSlotY));
    if (k < 0 || k >= kSlots || std::fabs(y - k * kSlotY) > kBand) return -1;
    return k;
}

int slot_of_seat(std::int64_t seat) {
    auto* p = seat == 1 ? world::p1_pawn() : core::pawn(seat);
    const auto at = p ? core::location(p) : std::nullopt;
    return at ? slot_of_y(at->y) : -1;
}

std::string map_id(int slot) {
    if (slot == 0) return ue::utf8(world::current_map_id());
    return slot > 0 && slot < kSlots ? g_slots[slot].map : std::string{};
}

float offset(int slot) { return slot > 0 ? slot * kSlotY : 0.0f; }

std::string tag_of_seat(std::int64_t seat) {
    auto it = g_seat_tag.find(seat);
    return it == g_seat_tag.end() ? std::string{} : it->second;
}

void tick(std::int64_t now_us) {
    static clock::Cadence every(250);
    if (!every.due(now_us)) return;
    // network seats at a door (their vanilla door overlap is ignored by the permission gate)
    for (const auto& seat : party::seats()) {
        if (seat.source != party::Source::Network || !core::permission::allows(seat.slot, core::permission::Gate::Travel)) continue;
        auto* pawn = core::pawn(seat.slot);
        const auto at = pawn ? core::location(pawn) : std::nullopt;
        if (!at) continue;
        for (const auto& e : registry::bucket(registry::Kind::TravelVolume)) {
            const auto& d = door_of(e);
            if (d.ok && std::fabs(at->y - d.center.y) <= d.extent.y && std::fabs(at->z - d.center.z) <= d.extent.z) {
                move_seat(seat.slot, d);
                break;
            }
        }
    }
    // pending arrivals
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (!world::level_instance::ready(g_slots[it->slot].map)) {
            ++it;
            continue;
        }
        auto at = player_start(it->slot, it->tag);
        if (!at) { // instance loaded but its PlayerStarts not registered yet
            ++it;
            continue;
        }
        place(it->seat, *at);
        ++g_moves;
        g_last = std::format("P{} arrived in {} (slot {})", it->seat, g_slots[it->slot].map, it->slot);
        it = g_pending.erase(it);
    }
    // handoff: host P1 now stands in a map that is also loaded as a background instance (P1 went through a
    // door into it): seats there move to the same local position in the foreground copy, then the background
    // copy goes (the vanilla load and ours use different instance names, so both exist until then)
    if (!party::transit::active()) {
        const auto fg = ue::utf8(world::current_map_id());
        for (int k = 1; k < kSlots; ++k) {
            if (g_slots[k].map.empty() || g_slots[k].map != fg) continue;
            for (const auto& seat : party::seats())
                if (slot_of_seat(seat.slot) == k)
                    if (auto* p = core::pawn(seat.slot))
                        if (auto at = core::location(p)) place(seat.slot, {at->x, at->y - offset(k), at->z}), ++g_moves;
            std::erase_if(g_pending, [k](const Pending& p) { return p.slot == k; });
            world::level_instance::unload(fg);
            g_slots[k] = {};
            ++g_unloads;
            g_last = std::format("handoff: P1 entered {} — seats moved to the foreground copy, background unloaded", fg);
            log::info("coop: {}", g_last);
        }
    }
    // grace unloads
    for (int k = 1; k < kSlots; ++k) {
        auto& sl = g_slots[k];
        if (sl.map.empty()) continue;
        bool used = false;
        for (const auto& seat : party::seats()) used |= slot_of_seat(seat.slot) == k;
        for (const auto& p : g_pending) used |= p.slot == k;
        if (used) {
            sl.empty_since = 0;
        } else if (!sl.empty_since) {
            sl.empty_since = now_us;
        } else if (now_us - sl.empty_since > kGraceUs) {
            world::level_instance::unload(sl.map);
            ++g_unloads;
            sl = {};
        }
    }
}

void before_foreground_switch(const std::string& target) {
    for (int k = 1; k < kSlots; ++k)
        if (g_slots[k].map == target) {
            world::level_instance::unload(target); // the vanilla load must not meet a second copy
            g_slots[k] = {};
            ++g_unloads;
            log::info("coop: host P1 enters {} — background instance unloaded first", target);
        }
}

void reset() {
    for (int k = 1; k < kSlots; ++k)
        if (!g_slots[k].map.empty()) world::level_instance::unload(g_slots[k].map);
    g_slots = {};
    g_pending.clear();
    g_doors.clear();
}

std::string describe() {
    std::string out = std::format("maps: moves={} loads={} unloads={} pending={} last='{}'\n", g_moves, g_loads, g_unloads, g_pending.size(), g_last);
    for (int k = 1; k < kSlots; ++k)
        if (!g_slots[k].map.empty()) out += std::format("  slot {} {} Y{:+.0f} starts={} ready={}{}\n", k, g_slots[k].map, offset(k), g_slots[k].starts.size(), world::level_instance::ready(g_slots[k].map), g_slots[k].empty_since ? " (empty)" : "");
    return out;
}

} // namespace elmp::coop::maps
