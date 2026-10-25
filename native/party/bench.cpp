#include "party/bench.h"

#include "party/seat.h"
#include "party/storage.h"
#include "party/transit.h"

#include "core/player.h"
#include "core/schema.h"
#include "core/vitals.h"

#include "input/virtual_pad.h"

#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"

#include "base/clock.h"
#include "base/events.h"
#include "base/log.h"

#include <cmath>
#include <map>
#include <unordered_map>

namespace elmp::party::bench {

namespace {

constexpr float kRadiusY = 150.0f, kRadiusZ = 250.0f;
constexpr std::int64_t kCooldownUs = 3'000'000;
constexpr std::uint32_t kInteract = 1u << 9; // ECommandInputTypes::INTERACT

struct Last {
    ObjHandle pawn;
    std::int64_t at = 0;
};
std::map<std::int64_t, Last> g_last;
std::unordered_map<std::int32_t, std::pair<std::int32_t, core::Vec3>> g_bench_pos; // index → (serial, position); benches do not move

bool near_bench(const core::Vec3& at) {
    const auto* s = core::schema::script();
    for (const auto& e : registry::bucket(registry::Kind::RestingPoint)) {
        auto* b = e.handle.get();
        if (!b || !s) continue;
        auto& cached = g_bench_pos[e.handle.index];
        if (cached.first != e.handle.serial) {
            reflect::Frame f(s->actor_location);
            if (!f.invoke(b)) continue;
            cached = {e.handle.serial, f.ret<core::Vec3>()};
        }
        if (std::abs(cached.second.y - at.y) <= kRadiusY && std::abs(cached.second.z - at.z) <= kRadiusZ) return true;
    }
    return false;
}

} // namespace

std::string rest(std::int64_t slot) {
    if (!party::seat(slot)) return "not an active PX seat";
    if (transit::active() || lifecycle::state() != lifecycle::State::Settled) return "world is loading";
    auto* p = core::pawn(slot);
    if (!p) return "player has no pawn";
    if (!core::vitals::read(slot).alive) return "player is dead";
    const auto pos = core::location(p);
    if (!pos || !near_bench(*pos)) return "not at a bench";
    const auto now = clock::now_us();
    auto& last = g_last[slot];
    if (last.pawn.get() == p && now - last.at < kCooldownUs) return "rest cooldown";
    if (auto err = core::vitals::restore(slot); !err.empty()) return "restore failed: " + err; // no cooldown recorded
    last = {ObjHandle::of(p), now};
    storage::capture(slot, true); // C11: resting stores the PX record
    events::bus().push(events::Type::Rested, static_cast<std::int32_t>(slot));
    log::info("party: P{} rested", slot);
    return {};
}

bool nearby(std::int64_t slot) {
    auto* p = core::pawn(slot);
    const auto pos = p ? core::location(p) : std::nullopt;
    return pos && near_bench(*pos);
}

void poll(std::int64_t slot) {
    if (!(input::pad::take_presses(slot) & kInteract)) return;
    if (auto why = rest(slot); !why.empty() && why != "not at a bench") log::info("party: P{} rest refused: {}", slot, why);
}

void forget(std::int64_t slot) { g_last.erase(slot); }

void install() {
    lifecycle::on_teardown([] {
        g_last.clear();
        g_bench_pos.clear();
    });
}

} // namespace elmp::party::bench
