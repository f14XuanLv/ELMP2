#include "modes/hunter.h"

#include "coop/host.h"
#include "coop/maps.h"
#include "coop/session.h"

#include "party/progress.h"
#include "party/respawn.h"
#include "party/seat.h"

#include "core/permission.h"
#include "core/player.h"
#include "core/profile.h"
#include "core/schema.h"
#include "core/vitals.h"

#include "overlay/render.h"

#include "engine/frame.h"
#include "engine/handle.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/log.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace elmp::modes::hunter {

namespace {

enum class State { Idle, Grace, Running, Ended };
const char* state_name(State s) {
    switch (s) {
    case State::Grace: return "grace";
    case State::Running: return "running";
    case State::Ended: return "ended";
    default: return "idle";
    }
}

struct HunterSeat {
    std::int64_t dead_since = 0;
    std::uint64_t deaths = 0, respawns = 0;
};

State g_state = State::Idle;
Options g_opt;
std::int64_t g_started_us = 0, g_running_us = 0, g_ended_us = 0;
int g_runner_deaths = 0;
bool g_runner_was_alive = true;
std::string g_winner, g_reason;
std::map<std::int64_t, HunterSeat> g_hunters;
std::vector<std::int32_t> g_boss_points; // handle indices of uncleared boss spawn points of the foreground map at start
std::string g_fg_map;

bool runner_alive() {
    const auto* s = core::schema::script();
    auto* p1 = world::p1_pawn();
    auto* hp = s && p1 ? s->zc_hp.object(p1) : nullptr;
    return !hp || s->hp_curr.read<std::int32_t>(hp) > 0; // no pawn (loading) ≠ death
}

bool cleared(ue::UObject* a) {
    static reflect::FuncRef is_cleared =
        reflect::Resolver("ClearableComponent", reflect::find_class(L"/Script/Zenith.ClearableComponent")).func(L"IsCleared", 0);
    static ObjHandle cls = ObjHandle::of(reflect::find_object(L"/Script/Zenith.ClearableComponent"));
    const auto* s = core::schema::script();
    if (!s || !is_cleared.ok()) return false;
    reflect::Frame f(s->actor_component_by_class);
    f.arg(0, cls.get());
    if (!f.invoke(a)) return false;
    auto* cc = f.ret<ue::UObject*>();
    if (!ue::alive(cc)) return false;
    reflect::Frame q(is_cleared);
    return q.invoke(cc) && q.ret_bool();
}

// uncleared boss spawn points of the foreground band (|Y| < 150000)
std::vector<std::int32_t> boss_points() {
    std::vector<std::int32_t> out;
    for (const auto& e : registry::bucket(registry::Kind::SpawnPoint)) {
        auto* a = e.handle.get();
        const auto at = a ? core::location(a) : std::nullopt;
        if (at && coop::maps::slot_of_y(at->y) == 0 && registry::class_name(e.class_id).find("Boss") != std::string::npos && !cleared(a))
            out.push_back(e.handle.index);
    }
    return out;
}

void end(const char* winner, std::string reason) {
    g_state = State::Ended;
    g_ended_us = clock::now_us();
    g_winner = winner;
    g_reason = std::move(reason);
    for (const auto& [slot, h] : g_hunters) party::respawn::release(slot);
    log::info("hunter: {} win — {}", g_winner, g_reason);
}

std::string clock_text(std::int64_t us) {
    const auto s = std::max<std::int64_t>(0, us / 1'000'000);
    return std::format("{}:{:02}", s / 60, s % 60);
}

std::string status_line() {
    const auto now = clock::now_us();
    switch (g_state) {
    case State::Grace: return std::format("HUNT  head start {}s", std::max<std::int64_t>(0, g_opt.grace_s - (now - g_started_us) / 1'000'000));
    case State::Running: {
        std::string t = clock_text(now - g_running_us);
        if (g_opt.time_limit_s > 0) t += " / " + clock_text(g_opt.time_limit_s * 1'000'000LL);
        return std::format("HUNT  {}  runner deaths {}/{}  target {}", t, g_runner_deaths, g_opt.kills, g_opt.target);
    }
    case State::Ended: return std::format("HUNT  {} win ({})", g_winner, g_reason);
    default: return {};
    }
}

std::string compass_for(std::int64_t slot) {
    if (g_state != State::Running && g_state != State::Grace) return {};
    auto* me = core::pawn(slot);
    auto* runner = world::p1_pawn();
    const auto a = me ? core::location(me) : std::nullopt;
    const auto b = runner ? core::location(runner) : std::nullopt;
    if (!a || !b) return {};
    const int ka = coop::maps::slot_of_y(a->y), kb = coop::maps::slot_of_y(b->y);
    if (ka != kb) return std::format("runner in {}", coop::maps::map_id(kb));
    const float dy = b->y - a->y, dz = b->z - a->z;
    const char* h = dy > 150 ? ">" : (dy < -150 ? "<" : "");
    const char* v = dz > 150 ? "^" : (dz < -150 ? "v" : "");
    return std::format("runner {}{} {:.0f}", h, v, std::sqrt(dy * dy + dz * dz));
}

void tick_frame(const frame::Tick& t) { tick(t.now_us); }

} // namespace

std::string start(const Options& o) {
    if (g_state == State::Grace || g_state == State::Running) return "a match is already running";
    if (lifecycle::state() != lifecycle::State::Settled) return "world is loading";
    std::vector<std::int64_t> hunters;
    for (const auto& s : party::seats()) hunters.push_back(s.slot);
    if (hunters.empty()) return "no hunters: join local seats or let guests connect first";
    if (hunters.size() > 4) return "at most 4 hunters";
    if (o.kills < 1 || o.grace_s < 0 || o.respawn_s < 0 || o.time_limit_s < 0) return "invalid options";
    g_opt = o;
    g_hunters.clear();
    for (auto slot : hunters) {
        if (auto e = party::set_team(slot, party::Team::Enemy); !e.empty()) return std::format("P{} team: {}", slot, e);
        core::permission::set(slot, core::permission::Gate::Travel, true);
        core::permission::set(slot, core::permission::Gate::Rest, false);
        if (o.preset)
            if (auto* pc = core::controller(slot)) core::profile::apply(pc, core::profile::preset(core::profile::Preset::Full), true);
        party::respawn::freeze(slot);
        g_hunters[slot] = {};
    }
    party::respawn::set_override([](std::int64_t slot) { return g_hunters.count(slot) != 0; });
    party::progress::set_mirror_only(o.preset ? nullptr : [](std::int64_t slot) { return g_hunters.count(slot) != 0; });
    g_state = State::Grace;
    g_started_us = clock::now_us();
    g_running_us = g_ended_us = 0;
    g_runner_deaths = 0;
    g_runner_was_alive = runner_alive();
    g_winner.clear(), g_reason.clear();
    g_fg_map = ue::utf8(world::current_map_id());
    g_boss_points = boss_points();
    log::info("hunter: match started — {} hunter(s), kills {}, time {} s, head start {} s, target {}, progress {}", hunters.size(), o.kills, o.time_limit_s,
              o.grace_s, o.target, o.preset ? "preset" : "mirror");
    return {};
}

void stop() {
    for (const auto& [slot, h] : g_hunters) {
        party::respawn::release(slot);
        party::set_team(slot, party::Team::Friendly);
    }
    g_hunters.clear();
    party::respawn::set_override(nullptr);
    party::progress::set_mirror_only(nullptr);
    g_state = State::Idle;
    overlay::set_banner({});
    if (coop::role() == coop::Role::Host) coop::host::broadcast_banner({});
}

bool active() { return g_state == State::Grace || g_state == State::Running; }
bool is_hunter(std::int64_t slot) { return g_hunters.count(slot) != 0; }
std::string compass(std::int64_t slot) { return is_hunter(slot) ? compass_for(slot) : std::string{}; }

void tick(std::int64_t now_us) {
    if (g_state == State::Idle) return;
    std::erase_if(g_hunters, [](const auto& kv) { return !party::seat(kv.first); }); // left the match
    if (g_state == State::Grace) {
        for (const auto& [slot, h] : g_hunters) party::respawn::freeze(slot);
        if (now_us - g_started_us >= g_opt.grace_s * 1'000'000LL) {
            for (const auto& [slot, h] : g_hunters) party::respawn::release(slot);
            g_state = State::Running;
            g_running_us = now_us;
            log::info("hunter: head start over — hunters released");
        }
    }
    if (g_state == State::Running && lifecycle::state() == lifecycle::State::Settled) {
        // runner deaths (falling edge; a missing pawn while loading is not a death)
        const bool alive = runner_alive();
        if (g_runner_was_alive && !alive) ++g_runner_deaths, log::info("hunter: runner died ({}/{})", g_runner_deaths, g_opt.kills);
        g_runner_was_alive = alive;
        if (g_runner_deaths >= g_opt.kills) end("hunters", std::format("runner killed {} time(s)", g_runner_deaths));
        else if (g_opt.time_limit_s > 0 && now_us - g_running_us >= g_opt.time_limit_s * 1'000'000LL) end("hunters", "time limit");
        else {
            const auto fg = ue::utf8(world::current_map_id());
            if (g_opt.target != "boss" && fg == g_opt.target) end("runner", "reached " + fg);
            else if (g_opt.target == "boss") {
                if (fg != g_fg_map) g_fg_map = fg, g_boss_points = boss_points(); // new map: its own boss points
                for (auto idx : g_boss_points)
                    for (const auto& e : registry::bucket(registry::Kind::SpawnPoint))
                        if (e.handle.index == idx)
                            if (auto* a = e.handle.get(); a && cleared(a)) end("runner", "boss defeated (" + ue::utf8(e.name) + ")");
            }
        }
        // hunters: death owned by the mode, respawn at the entrance of their own map
        for (auto& [slot, h] : g_hunters) {
            if (g_state != State::Running) break;
            const bool hunter_alive = core::pawn(slot) && party::respawn::alive(slot);
            if (hunter_alive) {
                h.dead_since = 0;
                continue;
            }
            if (!h.dead_since) h.dead_since = now_us, ++h.deaths;
            if (now_us - h.dead_since < g_opt.respawn_s * 1'000'000LL) continue;
            std::optional<core::Vec3> at = coop::maps::entrance_of_seat(slot);
            if (!at) at = core::location(world::p1_pawn()); // no pawn to locate: foreground entrance unknown → near P1
            if (!at) continue;
            if (auto e = party::respawn::revive(slot, *at); e.empty()) {
                h.dead_since = 0, ++h.respawns;
                core::vitals::restore(slot);
                log::info("hunter: P{} respawned at its map entrance", slot);
            } else {
                h.dead_since = now_us - g_opt.respawn_s * 1'000'000LL + 1'000'000; // retry in 1 s
            }
        }
    }
    // status (2 Hz): banner on the host + each guest gets its own compass
    static clock::Cadence every(500);
    if (!every.due(now_us)) return;
    const auto line = status_line();
    overlay::set_banner(line);
    if (coop::role() == coop::Role::Host)
        for (const auto& s : party::seats())
            if (s.source == party::Source::Network) {
                const auto c = compass(s.slot);
                coop::host::broadcast_banner(c.empty() ? line : line + "  |  " + c, s.slot);
            }
}

std::string describe() {
    std::string out = std::format("hunter: {} {}\n", state_name(g_state), status_line());
    out += std::format("  options: kills {} time {} s head start {} s respawn {} s target {} progress {}\n", g_opt.kills, g_opt.time_limit_s, g_opt.grace_s,
                       g_opt.respawn_s, g_opt.target, g_opt.preset ? "preset" : "mirror");
    out += std::format("  runner deaths {} bossPoints {}\n", g_runner_deaths, g_boss_points.size());
    for (const auto& [slot, h] : g_hunters)
        out += std::format("  P{} hunter deaths={} respawns={} {}\n", slot, h.deaths, h.respawns, compass_for(slot));
    return out;
}

void install() {
    frame::add_phase("modes.hunter", tick_frame, [] { return g_state != State::Idle; });
    lifecycle::on_teardown([] {
        if (g_state != State::Idle) g_boss_points.clear(); // handle indices die with the world
    });
}

} // namespace elmp::modes::hunter
