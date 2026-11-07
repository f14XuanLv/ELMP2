#include "party/respawn.h"

#include "party/bench.h"
#include "party/config.h"
#include "party/progress.h"
#include "party/rules.h"
#include "party/seat.h"
#include "party/transit.h"

#include "core/permission.h"
#include "core/schema.h"
#include "core/skin.h"
#include "core/spawn.h"

#include "input/virtual_pad.h"

#include "engine/frame.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/events.h"
#include "base/log.h"
#include "base/metrics.h"

#include <cmath>
#include <format>
#include <map>

namespace elmp::party::respawn {

namespace {

constexpr std::uint8_t kMoveNone = 0, kMoveFalling = 3;

struct Watch {
    std::int64_t dead_since = 0, far_since = 0;
    int revive_failures = 0;
};
std::map<std::int64_t, Watch> g_watch;
std::int64_t g_last_us = 0;
metrics::Counter& c_revived = metrics::counter("party.revived");
metrics::Counter& c_recalled = metrics::counter("party.recalled");

ue::UObject* movement(std::int64_t slot) {
    const auto* s = core::schema::script();
    auto* p = core::pawn(slot);
    return s && p ? s->char_movement.object(p) : nullptr;
}

} // namespace

bool alive(std::int64_t slot) {
    const auto* s = core::schema::script();
    auto* p = core::pawn(slot);
    auto* hp = s && p ? s->zc_hp.object(p) : nullptr;
    return hp && s->hp_curr.read<std::int32_t>(hp) > 0;
}

std::string teleport(std::int64_t slot, const core::Vec3& at) {
    const auto* s = core::schema::script();
    auto* p = core::pawn(slot);
    if (!s || !p) return "no pawn";
    reflect::Frame f(s->actor_teleport);
    f.arg(0, at).arg_bool(1, false).arg_bool(3, true);
    if (!f.invoke(p)) return "K2_SetActorLocation failed";
    if (auto* cm = movement(slot)) reflect::Frame(s->mc_stop).invoke(cm);
    return {};
}

void freeze(std::int64_t slot) {
    const auto* s = core::schema::script();
    auto* cm = movement(slot);
    if (!s || !cm) return;
    input::pad::set(slot, 0, 0, 0);
    reflect::Frame(s->mc_stop).invoke(cm);
    if (s->cm_mode.read<std::uint8_t>(cm) != kMoveNone) reflect::Frame(s->cm_disable).invoke(cm);
}

void release(std::int64_t slot) {
    const auto* s = core::schema::script();
    auto* cm = movement(slot);
    if (s && cm && s->cm_mode.read<std::uint8_t>(cm) == kMoveNone) reflect::Frame(s->cm_set_mode).arg(0, kMoveFalling).arg(1, std::uint8_t{0}).invoke(cm);
}

std::string revive(std::int64_t slot, const core::Vec3& at) {
    if (auto why = progress::sync(slot); !why.empty()) return "cannot revive before shared progress sync: " + why;
    core::spawn::Options o;
    o.location = at;
    o.skin = core::skin::current(core::pawn(slot));
    if (!core::skin::valid(o.skin)) o.skin = core::skin::kDefault; // P1e: from the seat's cold profile
    if (auto err = core::spawn::respawn(slot, o); !err.empty()) {
        core::spawn::despawn(slot, true); // no half-built pawn; the patrol retries
        return err;
    }
    core::permission::track(slot);
    ++c_revived;
    return {};
}

std::string place_at(std::int64_t slot, const core::Vec3& at, bool frozen, bool* revived) {
    if (revived) *revived = false;
    if (core::pawn(slot) && alive(slot)) {
        auto err = teleport(slot, at);
        if (!frozen) release(slot);
        return err;
    }
    if (!core::controller(slot)) return "no controller";
    if (auto err = revive(slot, at); !err.empty()) return err;
    if (revived) *revived = true;
    if (frozen) freeze(slot);
    return {};
}

bool (*g_override)(std::int64_t) = nullptr;

void set_override(bool (*owns)(std::int64_t slot)) { g_override = owns; }

void patrol(std::int64_t now_us) {
    g_last_us = now_us;
    if (lifecycle::state() != lifecycle::State::Settled || transit::active()) return;
    auto* p1 = world::p1_pawn();
    if (!p1) return;
    rules::tick();
    const auto p1_pos = core::location(p1);
    const bool p1_alive = [&] {
        const auto* s = core::schema::script();
        auto* hp = s ? s->zc_hp.object(p1) : nullptr;
        return hp && s->hp_curr.read<std::int32_t>(hp) > 0;
    }();
    int n = 0;
    for (const auto& seat : party::seats()) {
        ++n;
        const auto slot = seat.slot;
        auto& w = g_watch[slot];
        if (!core::controller(slot)) continue; // unknown / lost controller: never rebuilt blindly
        if (g_override && g_override(slot)) { // a mode (hunter) owns death / recall of this seat
            if (core::pawn(slot) && alive(slot)) bench::poll(slot);
            continue;
        }
        const bool has_pawn = core::pawn(slot) != nullptr;
        const bool is_alive = has_pawn && alive(slot);
        if (!is_alive) {
            if (!w.dead_since) {
                w.dead_since = now_us;
                events::bus().push(events::Type::Death, static_cast<std::int32_t>(slot), has_pawn ? 0 : 1);
            }
            if ((now_us - w.dead_since) / 1000 >= kDeathDelayMs) {
                const auto at = behind_p1(n);
                if (!at) continue;
                if (auto err = revive(slot, *at); err.empty()) {
                    w = Watch{};
                    log::info("party: P{} revived next to P1", slot);
                } else {
                    if (++w.revive_failures <= 3) log::warn("party: P{} revive failed (retrying): {}", slot, err);
                    w.dead_since = now_us - kDeathDelayMs * 1000 + 1'000'000; // retry in 1 s
                }
            }
            continue;
        }
        w.dead_since = 0;
        bench::poll(slot);
        if (!config::recall() || !p1_alive || !p1_pos) continue;
        const auto pos = core::location(core::pawn(slot));
        if (!pos) continue;
        const double dy = pos->y - p1_pos->y, dz = pos->z - p1_pos->z;
        const char* reason = nullptr;
        if (dz < -config::recall_fall_depth()) reason = "fell";
        else if (seat.source != Source::Network && std::sqrt(dy * dy + dz * dz) > config::recall_distance()) { // remote: fell only
            if (!w.far_since) w.far_since = now_us;
            if ((now_us - w.far_since) / 1000 >= static_cast<std::int64_t>(config::recall_delay_ms())) reason = "far";
        } else {
            w.far_since = 0;
        }
        if (reason) {
            if (const auto at = behind_p1(n); at && teleport(slot, *at).empty()) {
                release(slot);
                w.far_since = 0;
                ++c_recalled;
                events::bus().push(events::Type::Recalled, static_cast<std::int32_t>(slot));
                log::info("party: P{} recalled to P1 ({})", slot, reason);
            }
        }
    }
}

void forget(std::int64_t slot) { g_watch.erase(slot); }

std::string describe() {
    std::string out = std::format("revived={} recalled={}\n", c_revived.load(), c_recalled.load());
    for (const auto& [slot, w] : g_watch) out += std::format("P{}: dead_for={}ms far_for={}ms\n", slot, w.dead_since ? (g_last_us - w.dead_since) / 1000 : 0, w.far_since ? (g_last_us - w.far_since) / 1000 : 0);
    return out;
}

void install() {
    frame::add_phase("party.patrol", [](const frame::Tick& t) { patrol(t.now_us); }, [] { return party::count() > 0; }, 250);
    lifecycle::on_teardown([] {
        g_watch.clear();
        g_last_us = 0;
    });
}

} // namespace elmp::party::respawn
