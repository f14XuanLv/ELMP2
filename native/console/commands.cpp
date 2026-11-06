// P0 console commands: status / preflight / perf / registry / lifecycle / events / set / get.

#include "console/console.h"
#include "coop/session.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/binary_checks.h"
#include "engine/frame.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "base/events.h"
#include "base/hook.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/preflight.h"
#include "base/settings.h"

#include <elmp/build_info.h>

#include <format>
#include <vector>

#include <Windows.h>

namespace elmp::console {

namespace {

void line(std::string& out, std::string s) {
    out += s;
    out += '\n';
}

std::string summary_us(const metrics::Summary& s, std::string_view unit = "us") {
    return std::format("n={} mean={:.3f} p50={:.3f} p95={:.3f} p99={:.3f} max={:.3f} {}", s.n, s.mean, s.p50, s.p95, s.p99, s.max, unit);
}

void status(Args, std::string& out) {
    const auto& pf = preflight::current();
    line(out, std::format("ELMP {} ({}) api {} protocol {} ue4ss {}", build::version, build::dev ? "dev" : "shipping", build::api_core, build::protocol, build::ue4ss));
    line(out, std::format("preflight: {}{}", pf.ok ? "ok" : "FAILED: ", pf.reason));
    line(out, std::format("world: {} generation={} map={}{}", lifecycle::name(lifecycle::state()), lifecycle::generation(),
                          ue::utf8(world::current_map_id()), lifecycle::waiting_for().empty() ? "" : " (waiting: " + lifecycle::waiting_for() + ")"));
    const auto role = coop::role();
    line(out, std::format("frame: ticks={} idle={} active_slots={} session={}", frame::ticks(), frame::idle(), core::active(),
                          role == coop::Role::Host ? "host" : role == coop::Role::Client ? "client" : "none"));
    line(out, "core: " + core::schema::status());
    const auto st = registry::stats();
    std::string counts;
    for (std::size_t k = 0; k < registry::kKinds; ++k) counts += std::format(" {}={}", registry::kind_name(static_cast<registry::Kind>(k)), st.count[k]);
    line(out, "registry:" + counts);
    line(out, std::format("hooks armed: {}; reflect calls={} lookups={}", hook::armed().size(), reflect::calls(), reflect::lookups()));
}

void preflight_cmd(Args, std::string& out) {
    const auto& pf = preflight::current();
    line(out, std::format("exe: {}", pf.exe.string()));
    line(out, std::format("sha256: {} ({:.0f} ms)", pf.sha256, pf.ms));
    line(out, std::format("result: {}{}", pf.ok ? "ok" : "FAILED: ", pf.reason));
    for (const auto& f : pf.functions) line(out, std::format("  fn {:<26} rva 0x{:08X} {}", f.name, f.rva, f.ok ? "prefix ok" : f.reason));
    if (!binary_checks::ran()) line(out, "  offsets: not checked yet (after the world settles)");
    for (const auto& o : binary_checks::results()) line(out, std::format("  off {:<38} {:>5} {} ({})", o.name, o.value, o.ok ? "ok" : "FAILED", o.detail));
    const auto ws = world::schema_status();
    line(out, "world schema: " + (ws.empty() ? std::string("ok") : ws));
}

void perf(Args args, std::string& out) {
    if (!args.empty() && args[0] == "reset") {
        metrics::reset_histograms("frame.");
        metrics::reset_histograms("native_lily.");
        line(out, "frame histograms reset");
        return;
    }
    const auto snap = metrics::snapshot();
    for (const auto& [name, s] : snap.histograms)
        if (s.n) {
            const auto unit = name == "native_lily.component_rate" ? "x" : name == "native_lily.steps" ? "steps" : name.ends_with("_ms") ? "ms" : name.ends_with("_uu") ? "UU" : "us";
            line(out, std::format("{:<26} {}", name, summary_us(s, unit)));
        }
    for (const auto& [name, v] : snap.counters) line(out, std::format("{:<26} {}", name, v));
}

void registry_cmd(Args args, std::string& out) {
    const std::string sub = args.empty() ? "" : args[0];
    if (sub == "verify") {
        const auto r = registry::cross_check();
        line(out, std::format("verify: {} ({:.1f} ms)", r.consistent() ? "consistent" : "INCONSISTENT", r.ms));
        for (std::size_t k = 0; k < registry::kKinds; ++k)
            if (r.scanned[k] || r.registered[k])
                line(out, std::format("  {:<17} scanned={} registered={} missing={} extra={}", registry::kind_name(static_cast<registry::Kind>(k)), r.scanned[k],
                                      r.registered[k], r.missing[k], r.extra[k]));
        for (const auto& s : r.samples) line(out, "  " + s);
        return;
    }
    if (sub == "menu" && args.size() > 1) {
        registry::want_menu(args[1] == "on" || args[1] == "1");
        line(out, std::format("menu bucket {}", registry::menu_wanted() ? "maintained" : "off"));
        return;
    }
    if (sub == "list" && args.size() > 1) {
        for (std::size_t k = 0; k < registry::kKinds; ++k) {
            const auto kind = static_cast<registry::Kind>(k);
            if (args[1] != registry::kind_name(kind)) continue;
            const std::size_t limit = args.size() > 2 ? std::stoul(args[2]) : 20;
            std::size_t n = 0;
            for (const auto& e : registry::bucket(kind)) {
                if (n++ >= limit) break;
                line(out, std::format("  {} [{}] {}", ue::utf8(e.name), registry::class_name(e.class_id), e.handle.valid() ? "" : "(stale)"));
            }
            line(out, std::format("{} entries", registry::size(kind)));
            return;
        }
        line(out, "unknown kind");
        return;
    }
    const auto st = registry::stats();
    for (std::size_t k = 0; k < registry::kKinds; ++k) line(out, std::format("  {:<17} {}", registry::kind_name(static_cast<registry::Kind>(k)), st.count[k]));
    line(out, std::format("inserted={} removedEndPlay={} removedDeleted={} auditStale={} audits={} baselines={} baselineAdded={} baselineMs={:.1f} menu={}",
                          st.inserted, st.removed_endplay, st.removed_deleted, st.audit_stale, st.audits, st.baselines, st.baseline_added, st.baseline_ms,
                          registry::menu_wanted() ? "on" : "off"));
    line(out, "usage: elmp registry [verify | list <kind> [n] | menu on|off]");
}

void lifecycle_cmd(Args, std::string& out) {
    line(out, std::format("state={} generation={} teardowns={} waiting='{}'", lifecycle::name(lifecycle::state()), lifecycle::generation(), lifecycle::teardowns(),
                          lifecycle::waiting_for()));
    line(out, std::format("world={} p1={} pawn={} loader={} map={}", static_cast<void*>(world::world()), static_cast<void*>(world::p1_controller()),
                          static_cast<void*>(world::p1_pawn()), static_cast<void*>(world::world_loader()), ue::utf8(world::current_map_id())));
    line(out, "saved: " + log::utf8(world::saved_dir()));
}

void events_cmd(Args args, std::string& out) {
    const std::size_t n = args.empty() ? 20 : std::stoul(args[0]);
    std::vector<events::Event> buf(std::min<std::size_t>(n, events::Bus::kCapacity));
    const auto last = events::bus().last_seq();
    std::uint64_t cursor = last > buf.size() ? last - buf.size() : 0;
    const auto got = events::bus().since(cursor, buf);
    for (std::size_t i = 0; i < got; ++i)
        line(out, std::format("  #{} t={:.3f}s {} slot={} a={} b={}", buf[i].seq, buf[i].t_us / 1e6, events::name(buf[i].type), buf[i].slot, buf[i].a, buf[i].b));
    line(out, std::format("last seq {}", last));
}

void set_cmd(Args args, std::string& out) {
    if (args.size() < 2) return line(out, "usage: elmp set <key> <value>");
    const auto err = settings::set(args[0], args[1]);
    line(out, err.empty() ? args[0] + " = " + args[1] : "refused: " + err);
}

void native_cmd(Args args, std::string& out) {
    if (args.empty()) return line(out, "usage: elmp native </Script/Pkg.Class:Function> [...]");
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (const auto& a : args) {
        auto* fn = static_cast<ue::UFunction*>(reflect::find_object(log::wide(a)));
        if (!fn) {
            line(out, a + ": not found");
            continue;
        }
        const auto p = reinterpret_cast<std::uintptr_t>(fn->GetFuncPtr());
        line(out, std::format("{}: exec rva 0x{:X} flags 0x{:08X}", a, p - base, static_cast<std::uint32_t>(fn->GetFunctionFlags())));
    }
}

void get_cmd(Args args, std::string& out) {
    for (const auto& e : settings::all()) {
        if (!args.empty() && args[0] != e.spec.key) continue;
        line(out, std::format("  {} = {}{}  {}", e.spec.key, settings::to_text(e.value), e.spec.persist ? "" : " (transient)", e.spec.help));
    }
}

} // namespace

void register_p0_commands() {
    add("status", "version, preflight, world, registry summary", status);
    add("preflight", "EXE hash, function prefixes, raw offset checks", preflight_cmd);
    add("perf", "frame / phase timings and counters [reset]", perf);
    add("registry", "object registry [verify | list <kind> [n] | menu on|off]", registry_cmd);
    add("lifecycle", "world state machine and P1 resolution", lifecycle_cmd);
    add("events", "recent event bus entries [n]", events_cmd);
    add("set", "<key> <value> (persisted unless transient)", set_cmd);
    add("get", "[key] list settings", get_cmd);
    add("native", "<UFunction path> [...]  (Dev) exec thunk RVA for IDA", native_cmd);
}

} // namespace elmp::console
