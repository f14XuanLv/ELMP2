#include "engine/frame.h"

#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/ue.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/log.h"
#include "base/metrics.h"

#include <Unreal/Hooks/Hooks.hpp>

#include <string>
#include <vector>

namespace elmp::frame {

using namespace RC::Unreal;

namespace {

struct Phase {
    const char* name;
    PhaseFn fn;
    Predicate enabled;
    clock::Cadence cadence;
    bool periodic;
    metrics::Histogram* hist;
};

std::vector<Phase> g_phases;
Predicate g_seats = nullptr, g_session = nullptr;
Hook::GlobalCallbackId g_hook{};
bool g_installed = false;
std::uint64_t g_ticks = 0;
std::int64_t g_last_us = 0;
float g_delta = 0;

metrics::Histogram& h_elmp = metrics::histogram("frame.elmp_us");
metrics::Histogram& h_idle = metrics::histogram("frame.idle_elmp_us");
metrics::Histogram& h_interval = metrics::histogram("frame.interval_us");
metrics::Counter& c_idle_calls = metrics::counter("g0.reflect_calls");
metrics::Counter& c_idle_lookups = metrics::counter("g0.reflect_lookups");
metrics::Counter& c_idle_ticks = metrics::counter("g0.idle_ticks");
metrics::Counter& c_exceptions = metrics::counter("frame.exceptions");

void on_tick(UEngine* engine, float dt) {
    g_delta = dt;
    const auto t0 = clock::now_us();
    if (!g_ticks) ue::mark_game_thread();
    ++g_ticks;
    if (g_last_us) h_interval.add(static_cast<float>(t0 - g_last_us));
    g_last_us = t0;
    world::set_engine(engine);

    const bool was_idle = idle();
    const auto calls0 = reflect::calls(), lookups0 = reflect::lookups();
    const Tick tick{t0, dt};
    for (auto& p : g_phases) {
        if (p.enabled && !p.enabled()) continue;
        if (p.periodic && !p.cadence.due(t0)) continue;
        const auto s = clock::now_us();
        try {
            p.fn(tick);
        } catch (const std::exception& e) {
            ++c_exceptions;
            log::error("frame phase {} threw: {}", p.name, e.what());
        }
        p.hist->add(static_cast<float>(clock::now_us() - s));
    }
    const auto spent = static_cast<float>(clock::now_us() - t0);
    h_elmp.add(spent);
    if (was_idle && idle()) {
        ++c_idle_ticks;
        h_idle.add(spent);
        c_idle_calls += reflect::calls() - calls0;
        c_idle_lookups += reflect::lookups() - lookups0;
    }
}

} // namespace

void add_phase(const char* name, PhaseFn fn, Predicate enabled, std::int64_t period_ms) {
    g_phases.push_back(Phase{name, fn, enabled, clock::Cadence(period_ms), period_ms > 0, &metrics::histogram(std::string("frame.") + name + "_us")});
}

void set_activity(Predicate seats, Predicate session) {
    g_seats = seats;
    g_session = session;
}

bool idle() {
    return lifecycle::state() == lifecycle::State::Settled && !(g_seats && g_seats()) && !(g_session && g_session());
}

void install() {
    if (g_installed) return;
    g_installed = true;
    Hook::FCallbackOptions opt;
    opt.OwnerModName = STR("ELMP");
    opt.HookName = STR("frame.pump");
    g_hook = Hook::RegisterEngineTickPreCallback([](auto&, UEngine* engine, float dt, bool) { on_tick(engine, dt); }, opt);
}

void uninstall() {
    if (!g_installed) return;
    Hook::UnregisterCallback(g_hook);
    g_installed = false;
}

std::uint64_t ticks() { return g_ticks; }
float delta() { return g_delta; }

} // namespace elmp::frame
