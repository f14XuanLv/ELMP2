// LilyCore console commands (P1a): spawn / remove / respawn a Lily on a slot, profiles, vitals, skins and
// the virtual pad. Slot 1 (P1) is readable; writes to it are refused by the core. Test slots 101..1000
// are leased to "console" (Dev builds only); 2..100 to ELMP.

#include "console/console.h"

#include "core/player.h"
#include "core/profile.h"
#include "core/schema.h"
#include "core/skin.h"
#include "core/spawn.h"
#include "core/vitals.h"

#include "input/virtual_pad.h"

#include "engine/frame.h"
#include "engine/reflect.h"

#include "base/clock.h"
#include "base/json.h"
#include "base/metrics.h"

#include <elmp/build_info.h>

#include <charconv>
#include <format>
#include <map>

namespace elmp::console {

namespace {

void line(std::string& out, std::string s) {
    out += s;
    out += '\n';
}

std::int64_t slot_arg(Args a, std::size_t i) {
    if (i >= a.size()) throw std::runtime_error("missing slot");
    std::int64_t v = 0;
    const auto& s = a[i];
    if (std::from_chars(s.data(), s.data() + s.size(), v).ec != std::errc{} || v < 1) throw std::runtime_error("bad slot '" + s + "'");
    return v;
}

double num_arg(Args a, std::size_t i) {
    if (i >= a.size()) throw std::runtime_error("missing number");
    return std::stod(a[i]);
}

std::string join(Args a, std::size_t from) {
    std::string s;
    for (std::size_t i = from; i < a.size(); ++i) s += (i > from ? " " : "") + a[i];
    return s;
}

std::uint32_t bits_arg(const std::string& spec) {
    std::uint32_t bits = 0;
    std::size_t start = 0;
    while (start <= spec.size()) {
        auto end = spec.find('+', start);
        if (end == std::string::npos) end = spec.size();
        const auto c = input::command_named(std::string_view(spec).substr(start, end - start));
        if (!c) throw std::runtime_error("unknown command '" + spec.substr(start, end - start) + "' (JUMP DODGE ATTACK SUMMON_1 SUMMON_2 SUMMON_SET_SWITCH HOOK HEAL INTERACT)");
        bits |= 1u << static_cast<int>(*c);
        start = end + 1;
    }
    return bits;
}

void must(const std::string& err) {
    if (!err.empty()) throw std::runtime_error(err);
}

// Resolves the profile argument: preset name, "mirror" (P1's current profile) or inline JSON.
core::profile::Profile profile_arg(Args a, std::size_t i) {
    if (i >= a.size()) throw std::runtime_error("missing profile (baseline|default|full|max|mirror|{json})");
    if (auto p = core::profile::preset_named(a[i])) return core::profile::preset(*p);
    if (a[i] == "mirror") {
        std::string err;
        auto p = core::profile::read(core::controller(1), &err);
        if (!p) throw std::runtime_error("cannot read P1: " + err);
        return *p;
    }
    json::Value v;
    std::string err;
    const auto text = join(a, i);
    if (!json::parse(text, v, &err)) throw std::runtime_error("profile json: " + err);
    core::profile::Profile p;
    if (!core::profile::from_json(v, p, &err)) throw std::runtime_error(err);
    return p;
}

void core_cmd(Args, std::string& out) {
    line(out, "schema: " + core::schema::status());
    out += core::describe();
    std::string ls;
    for (const auto& [slot, owner] : core::leases().leases()) ls += std::format(" {}={}", slot, owner);
    line(out, "leases:" + (ls.empty() ? std::string(" none") : ls));
    line(out, std::format("pads attached: {}; P1 injection {}", input::pad::attached(), input::pad::player1_allowed() ? "allowed" : "off"));
}

void spawn_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    if (slot < 2) throw std::runtime_error("slot 1 is the original player");
    const auto prof = a.size() > 1 ? profile_arg(a, 1) : core::profile::preset(core::profile::Preset::Default);
    core::spawn::Options opt;
    if (a.size() > 2 && core::profile::preset_named(a[1])) opt.skin = std::stoi(a[2]);
    const auto owner = core::owner_for(slot);
    const bool had_lease = core::leases().owner_of(slot).has_value();
    if (const auto r = core::leases().acquire(owner, slot); r != slots::Result::Ok) throw std::runtime_error(std::format("lease {}: {}", slot, slots::describe(r)));
    if (auto err = core::spawn::spawn(slot, opt); !err.empty()) {
        if (!had_lease) core::leases().release(owner, slot);
        throw std::runtime_error(err);
    }
    const auto report = core::profile::reset(core::controller(slot), prof);
    out += report.text();
    const auto rest = core::vitals::restore(slot);
    line(out, std::format("slot {} spawned (owner {}, skin {}){}", slot, owner.name, core::skin::current(core::pawn(slot)), rest.empty() ? "" : "; restore: " + rest));
}

void despawn_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    const bool keep = a.size() > 1 && a[1] == "keep";
    must(core::spawn::despawn(slot, keep));
    if (!keep) core::leases().release(core::owner_for(slot), slot);
    line(out, std::format("slot {} removed{}", slot, keep ? " (equipment kept, lease kept)" : ""));
}

void respawn_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    core::spawn::Options opt;
    opt.skin = core::skin::current(core::pawn(slot));
    if (!core::skin::valid(opt.skin)) opt.skin = core::skin::kDefault;
    must(core::spawn::respawn(slot, opt));
    line(out, std::format("slot {} respawned", slot));
}

void lily_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    const auto st = core::state(slot);
    if (!st.valid) return line(out, std::format("slot {}: no pawn", slot));
    const auto pad = input::pad::info(slot);
    line(out, std::format("slot {}: pos=({:.1f},{:.1f},{:.1f}) anim={} t={:.2f} alive={} busy={} hp={}/{} mp={}/{} heals={}/{} skin={} pad={}", slot, st.pos.x, st.pos.y,
                          st.pos.z, st.animation.empty() ? "?" : st.animation, st.track_time, st.alive, st.busy, st.hp, st.max_hp, st.mp, st.max_mp, st.heals,
                          st.max_heals, core::skin::current(core::pawn(slot)),
                          pad.attached ? std::format("x={:.2f} y={:.2f} raw={} frames={}", pad.x, pad.y, input::bits_text(pad.raw), pad.frames) : "none"));
}

void profile_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    auto* pc = core::controller(slot);
    const std::string sub = a.size() > 1 ? a[1] : "";
    if (sub.empty() || sub == "json") {
        std::string err;
        const auto p = core::profile::read(pc, &err);
        if (!p) throw std::runtime_error(err);
        return line(out, json::dump(core::profile::to_json(*p), sub == "json" ? 0 : 2));
    }
    if (sub == "apply") {
        bool exact = !a.empty() && a.back() == "exact";
        const std::vector<std::string> rest(a.begin(), exact ? a.end() - 1 : a.end());
        out += core::profile::apply(pc, profile_arg(rest, 2), exact).text();
        return;
    }
    if (sub == "reset") {
        out += core::profile::reset(pc, profile_arg(a, 2)).text();
        return;
    }
    throw std::runtime_error("usage: elmp profile <slot> [json|apply <p> [exact]|reset <p>]");
}

void vitals_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    if (a.size() > 1 && a[1] == "restore") must(core::vitals::restore(slot));
    const auto v = core::vitals::read(slot);
    line(out, std::format("slot {}: valid={} alive={} hp={}/{} mp={}/{} heals={}/{}", slot, v.valid, v.alive, v.hp, v.max_hp, v.mp, v.max_mp, v.heals, v.max_heals));
}

void skin_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    auto* p = core::pawn(slot);
    if (a.size() > 1) {
        if (slot < 2 || !core::is_elmp_controller(core::controller(slot))) throw std::runtime_error("P1 is read-only");
        must(core::skin::apply(p, std::stoi(a[1])));
    }
    line(out, std::format("slot {} skin {}", slot, core::skin::current(p)));
}

void pad_cmd(Args a, std::string& out) {
    if (!a.empty() && a[0] == "allow1") {
        const bool on = a.size() > 1 && (a[1] == "on" || a[1] == "1");
        must(input::pad::allow_player1(on));
        if (on) must(input::pad::attach(1, core::pawn(1), core::controller(1)));
        return line(out, std::format("P1 injection {}", on ? "allowed + attached" : "off"));
    }
    const auto slot = slot_arg(a, 0);
    const auto i = input::pad::info(slot);
    line(out, i.attached ? std::format("slot {}: x={:.2f} y={:.2f} raw={} presses={} frames={}", slot, i.x, i.y, input::bits_text(i.raw), input::bits_text(i.presses), i.frames)
                         : std::format("slot {}: no virtual pad", slot));
}

void move_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    must(input::pad::axes(slot, num_arg(a, 1), num_arg(a, 2)));
    line(out, std::format("slot {} axes {} {}", slot, a[1], a[2]));
}

void hold_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    if (a.size() < 2) throw std::runtime_error("usage: elmp hold <slot> <CMD[+CMD]> [1|0]");
    const bool down = !(a.size() > 2 && (a[2] == "0" || a[2] == "off"));
    must(input::pad::hold(slot, bits_arg(a[1]), down));
    line(out, std::format("slot {} {} {}", slot, down ? "holds" : "releases", a[1]));
}

void press_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    if (a.size() < 2) throw std::runtime_error("usage: elmp press <slot> <CMD[+CMD]>");
    must(input::pad::tap(slot, bits_arg(a[1])));
    line(out, std::format("slot {} taps {}", slot, a[1]));
}

void stop_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    must(input::pad::set(slot, 0, 0, 0));
    line(out, std::format("slot {} stopped", slot));
}

#ifdef ELMP_DEV
// Animation / position trace (Dev observation, F5.2): every frame for <ms>, records each change of the
// track-0 animation with its time offset and position. Active only while a trace runs (frame phase gated).
struct Trace {
    std::int64_t slot = 0, start_us = 0, end_us = 0;
    std::string last;
    std::vector<std::string> lines;
};
Trace g_trace;

void trace_phase(const frame::Tick& t) {
    const auto st = core::state(g_trace.slot);
    const auto anim = st.valid ? st.animation : std::string("<no pawn>");
    if (anim != g_trace.last) {
        g_trace.last = anim;
        g_trace.lines.push_back(std::format("+{:>5} ms {} pos=({:.0f},{:.0f})", (t.now_us - g_trace.start_us) / 1000, anim, st.pos.y, st.pos.z));
    }
    if (t.now_us >= g_trace.end_us) {
        g_trace.lines.push_back(std::format("+{:>5} ms end pos=({:.0f},{:.0f})", (t.now_us - g_trace.start_us) / 1000, st.pos.y, st.pos.z));
        g_trace.end_us = 0;
    }
}

void trace_cmd(Args a, std::string& out) {
    const auto slot = slot_arg(a, 0);
    if (a.size() > 1) {
        const auto now = clock::now_us();
        g_trace = Trace{slot, now, now + static_cast<std::int64_t>(num_arg(a, 1) * 1000), "", {}};
        return line(out, std::format("tracing slot {} for {} ms", slot, a[1]));
    }
    line(out, std::format("trace slot {} ({}):", g_trace.slot, g_trace.end_us ? "running" : "done"));
    for (const auto& l : g_trace.lines) line(out, "  " + l);
}
#endif

} // namespace

void register_core_commands() {
    add("core", "core schema, slots, leases, pads", core_cmd);
    add("spawn", "<slot> [baseline|default|full|max|mirror|{json}] [skin 1..6]", spawn_cmd);
    add("despawn", "<slot> [keep]  remove (unequip + release lease; keep = respawn-style)", despawn_cmd);
    add("respawn", "<slot>  replace the pawn only (ReequipSpirits + restore)", respawn_cmd);
    add("lily", "<slot>  position / animation / vitals / pad", lily_cmd);
    add("profile", "<slot> [json|apply <p> [exact]|reset <p>]", profile_cmd);
    add("vitals", "<slot> [restore]", vitals_cmd);
    add("skin", "<slot> [1..6]", skin_cmd);
    add("pad", "<slot> | allow1 on|off (Dev)", pad_cmd);
    add("move", "<slot> <x> <y>  virtual pad axes", move_cmd);
    add("hold", "<slot> <CMD[+CMD]> [1|0]", hold_cmd);
    add("press", "<slot> <CMD[+CMD]>  one press edge", press_cmd);
    add("stop", "<slot>  axes 0, release all", stop_cmd);
#ifdef ELMP_DEV
    add("trace", "<slot> [ms]  start / read an animation trace (Dev)", trace_cmd);
    frame::add_phase("dev.trace", trace_phase, [] { return g_trace.end_us != 0; });
#endif
}

} // namespace elmp::console
