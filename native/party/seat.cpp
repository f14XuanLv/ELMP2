#include "party/seat.h"

#include "party/bench.h"
#include "party/camera_sync.h"
#include "party/devices.h"
#include "party/progress.h"
#include "party/storage.h"
#include "party/respawn.h"
#include "party/transit.h"

#include "coop/session.h"

#include "core/hitrules.h"
#include "core/permission.h"
#include "core/profile.h"
#include "core/schema.h"
#include "core/spawn.h"
#include "core/vitals.h"

#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/log.h"

#include <format>
#include <map>
#include <set>

namespace elmp::party {

namespace {

std::map<std::int64_t, Seat> g_seats;

constexpr float kSpacing = 70.0f, kLift = 10.0f;
constexpr int kCycle = 4;

core::hitrules::Team to_rules(Team t) { return t == Team::Enemy ? core::hitrules::Team::Enemy : core::hitrules::Team::Friendly; }

void sync_router() {
    std::set<std::int64_t> device_seats;
    for (const auto& [slot, s] : g_seats)
        if (s.source == Source::Devices) device_seats.insert(slot);
    devices::sync_seats(device_seats);
}

void on_teardown() {
    if (!g_seats.empty()) log::info("party: {} seat(s) dropped with the world (stored ones rejoin when their save loads)", g_seats.size());
    g_seats.clear();
    progress::forget_all();
    sync_router();
}

} // namespace

const char* source_name(Source s) noexcept { return s == Source::Script ? "script" : s == Source::Network ? "network" : "devices"; }
const char* team_name(Team t) noexcept { return t == Team::Enemy ? "enemy" : "friendly"; }

std::optional<core::Vec3> behind_p1(int n) {
    const auto* s = core::schema::script();
    auto* p1 = world::p1_pawn();
    if (!s || !p1) return std::nullopt;
    reflect::Frame loc(s->actor_location);
    if (!loc.invoke(p1)) return std::nullopt;
    auto at = loc.ret<core::Vec3>();
    bool facing_right = true;
    if (auto* loco = s->zc_locomotion.object(p1)) {
        reflect::Frame f(s->loco_facing_right);
        if (f.invoke(loco)) facing_right = f.ret_bool();
    }
    const float d = kSpacing * static_cast<float>((n - 1) % kCycle + 1);
    at.y += facing_right ? -d : d;
    at.z += kLift;
    return at;
}

std::string join(const JoinOptions& opt, std::int64_t* slot_out) {
    if (lifecycle::state() != lifecycle::State::Settled) return "world not settled";
    // FR-NET-3d: a guest process has no local seats (its world is a presentation of the host's)
    if (opt.source != Source::Network && coop::role() == coop::Role::Client) return "local players are disabled while joined to a host";
    if (!world::p1_pawn()) return "the original player is not in the world";
    if (auto e = storage::join_allowed(); !e.empty()) return e;
    std::int64_t slot = opt.slot;
    if (slot == 0) {
        for (std::int64_t s = slots::kInternalFirst; s <= slots::kInternalLast && !slot; ++s)
            if (!g_seats.contains(s) && !core::leases().owner_of(s)) slot = s;
        if (!slot) return "no free seat (2..100)";
    }
    if (slot < slots::kInternalFirst || slot > slots::kInternalLast) return "local seats use slots 2..100";
    if (g_seats.contains(slot)) return std::format("seat {} is taken", slot);
    const slots::Owner owner{"ELMP", slots::OwnerKind::Elmp};
    if (const auto r = core::leases().acquire(owner, slot); r != slots::Result::Ok) return std::format("lease {}: {}", slot, slots::describe(r));

    core::spawn::Options so;
    so.location = behind_p1(static_cast<int>(g_seats.size()) + 1);
    const auto fail = [&](std::string why) {
        core::spawn::despawn(slot, false);
        core::leases().release(owner, slot);
        progress::forget(slot);
        return "seat initialization failed: " + why;
    };
    if (auto err = core::spawn::spawn(slot, so); !err.empty()) {
        core::leases().release(owner, slot);
        return err;
    }
    // Baseline: shared progress from P1 + the seat's own equipment (default until save_scope restores it).
    std::string err;
    auto base = progress::shared_profile(&err);
    if (!base) return fail("shared progress unavailable: " + err);
    base->spirits = core::profile::preset(core::profile::Preset::Default).spirits;
    base->summon_set = 1;
    if (base->passives) base->passives->equipped = std::vector<std::string>{};
    if (const auto report = core::profile::reset(core::controller(slot), *base); !report.ok()) return fail(report.text());
    progress::joined(slot);
    if (auto e = core::vitals::restore(slot); !e.empty()) return fail("restore: " + e);

    if (auto e = core::hitrules::bind(owner, slot, to_rules(opt.team)); !e.empty()) return fail("team: " + e);
    g_seats[slot] = Seat{slot, opt.source, opt.team};
    core::permission::track(slot);
    transit::arm();
    sync_router();
    if (opt.source == Source::Devices) {
        if (auto e = devices::auto_assign(slot, opt.pad); !e.empty()) log::warn("seat {}: device assignment: {}", slot, e);
        sync_router();
    }
    storage::capture(slot, true); // skipped while storage itself restores this seat
    log::info("party: seat {} joined ({}, {})", slot, source_name(opt.source), team_name(opt.team));
    if (slot_out) *slot_out = slot;
    return {};
}

std::string leave(std::int64_t slot) {
    if (!g_seats.contains(slot)) return std::format("no seat {}", slot);
    storage::capture(slot, false); // equipment kept for the next join
    progress::forget(slot);
    respawn::forget(slot);
    bench::forget(slot);
    core::permission::forget(slot);
    core::hitrules::unbind(slot);
    const auto err = core::spawn::despawn(slot, false);
    core::leases().release({"ELMP", slots::OwnerKind::Elmp}, slot);
    g_seats.erase(slot);
    camera_sync::left(slot);
    sync_router();
    log::info("party: seat {} left{}", slot, err.empty() ? "" : " (" + err + ")");
    return err;
}

std::string set_team(std::int64_t slot, Team team) {
    auto it = g_seats.find(slot);
    if (it == g_seats.end()) return std::format("no seat {}", slot);
    if (auto e = core::hitrules::bind({"ELMP", slots::OwnerKind::Elmp}, slot, to_rules(team)); !e.empty()) return e;
    it->second.team = team;
    storage::capture(slot, true);
    log::info("party: seat {} is now {}", slot, team_name(team));
    return {};
}

const Seat* seat(std::int64_t slot) {
    auto it = g_seats.find(slot);
    return it == g_seats.end() ? nullptr : &it->second;
}

std::vector<Seat> seats() {
    std::vector<Seat> out;
    for (const auto& [slot, s] : g_seats) out.push_back(s);
    return out;
}

std::size_t count() { return g_seats.size(); }

void install() { lifecycle::on_teardown(on_teardown); }

} // namespace elmp::party
