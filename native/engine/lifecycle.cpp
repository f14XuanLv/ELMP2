#include "engine/lifecycle.h"

#include "engine/registry.h"
#include "engine/ue.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/events.h"
#include "base/log.h"

#include <Unreal/Hooks/Hooks.hpp>

#include <vector>

namespace elmp::lifecycle {

using namespace RC::Unreal;

namespace {

State g_state = State::Boot;
std::uint64_t g_generation = 0, g_teardowns = 0;
std::int64_t g_settled_at = 0;
std::string g_waiting = "boot";
clock::Cadence g_poll(250);
std::vector<Callback> g_teardown_cbs, g_settled_cbs;
Hook::GlobalCallbackId g_loadmap{};
bool g_installed = false;

void enter(State s) {
    if (g_state == s) return;
    log::info("world {} -> {}", name(g_state), name(s));
    g_state = s;
}

void teardown() {
    if (g_state == State::Settled) {
        enter(State::TearingDown);
        events::bus().push(events::Type::WorldTeardown, 0, static_cast<std::int64_t>(g_generation));
        for (auto cb : g_teardown_cbs) cb();
        ++g_teardowns;
    }
    registry::clear();
    world::retry_schema();
    enter(State::Loading);
    g_waiting = "loading map";
    g_poll.reset();
}

void poll() {
    if (!world::world()) {
        g_waiting = world::schema_status().empty() ? "no world" : "world schema: " + world::schema_status();
        return enter(State::Loading);
    }
    if (!world::world_loader()) {
        g_waiting = "no WorldLoaderSubsystem";
        return enter(State::Loading);
    }
    if (world::current_map_id() == FName()) {
        g_waiting = world::p1_controller() ? "title (no game map)" : "no P1 controller";
        return enter(world::p1_controller() ? State::Title : State::Loading);
    }
    if (!world::p1_pawn()) {
        g_waiting = "P1 pawn missing";
        return enter(State::Settling);
    }
    bool loading = true, ready = false;
    if (!world::loader_flags(loading, ready) || loading || !ready) {
        g_waiting = loading ? "map loading" : "map not ready";
        return enter(State::Settling);
    }
    g_waiting.clear();
    enter(State::Settled);
    ++g_generation;
    g_settled_at = clock::now_us();
    registry::baseline();
    events::bus().push(events::Type::WorldSettled, 0, static_cast<std::int64_t>(g_generation));
    for (auto cb : g_settled_cbs) cb();
    log::info("world settled (generation {}, map {})", g_generation, ue::utf8(world::current_map_id()));
}

} // namespace

const char* name(State s) noexcept {
    switch (s) {
    case State::Boot: return "boot";
    case State::Title: return "title";
    case State::Loading: return "loading";
    case State::Settling: return "settling";
    case State::Settled: return "settled";
    case State::TearingDown: return "tearingDown";
    }
    return "?";
}

void on_teardown(Callback cb) { g_teardown_cbs.push_back(cb); }
void on_settled(Callback cb) { g_settled_cbs.push_back(cb); }

void install() {
    if (g_installed) return;
    g_installed = true;
    Hook::FCallbackOptions opt;
    opt.OwnerModName = STR("ELMP");
    opt.HookName = STR("lifecycle.loadMap");
    g_loadmap = Hook::RegisterLoadMapPreCallback([](auto&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) { teardown(); }, opt);
}

void uninstall() {
    if (!g_installed) return;
    Hook::UnregisterCallback(g_loadmap);
    g_installed = false;
}

void step(std::int64_t now) {
    if (g_state == State::Settled) return;
    if (g_state == State::Boot) enter(State::Loading);
    if (g_poll.due(now)) poll();
}

State state() { return g_state; }
std::uint64_t generation() { return g_generation; }
const std::string& waiting_for() { return g_waiting; }
std::int64_t settled_at_us() { return g_settled_at; }
std::uint64_t teardowns() { return g_teardowns; }

} // namespace elmp::lifecycle
