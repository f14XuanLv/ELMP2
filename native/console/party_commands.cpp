// Local-multiplayer console commands (P1b): seats, devices, bindings, mute. The manager GUI (P1d) calls the
// same functions; there are no global hotkeys (FR-LOC-3b).

#include "console/console.h"

#include "party/bench.h"
#include "party/camera_sync.h"
#include "party/hud_feed.h"
#include "party/manager.h"
#include "party/menu_ctrl.h"
#include "pvp/hitgate.h"
#include "overlay/render.h"
#include "party/devices.h"
#include "party/progress.h"
#include "party/respawn.h"
#include "party/rules.h"
#include "party/seat.h"
#include "party/transit.h"

#include "core/permission.h"
#include "core/player.h"
#include "core/schema.h"

#include "engine/reflect.h"

#include "input/bindings.h"
#include "input/rawinput.h"
#include "input/router.h"
#include "input/virtual_pad.h"
#include "input/xinput.h"

#include "base/settings.h"

#include <charconv>
#include <format>

namespace elmp::console {

namespace {

void line(std::string& out, std::string s) {
    out += s;
    out += '\n';
}

void must(const std::string& err) {
    if (!err.empty()) throw std::runtime_error(err);
}

std::int64_t to_int(const std::string& s) {
    std::int64_t v = 0;
    if (std::from_chars(s.data(), s.data() + s.size(), v).ec != std::errc{}) throw std::runtime_error("bad number '" + s + "'");
    return v;
}

// "pad 1" / "xinput:1" / "kbd#2" (index in `elmp devices`) / a full device id.
std::string device_arg(Args a, std::size_t& i) {
    if (i >= a.size()) throw std::runtime_error("missing device");
    const auto& s = a[i++];
    if (s == "pad") {
        if (i >= a.size()) throw std::runtime_error("missing pad index");
        return input::router::pad_id(static_cast<int>(to_int(a[i++])));
    }
    if (s.starts_with("kbd#")) {
        const auto n = to_int(s.substr(4));
        std::int64_t k = 0;
        for (const auto& d : input::rawinput::devices())
            if (d.kind == "keyboard" && ++k == n) return d.id;
        throw std::runtime_error("no keyboard #" + s.substr(4));
    }
    return s;
}

std::string slot_label(std::int64_t s) { return s == 0 ? "muted" : s == 1 ? "P1" : std::format("P{}", s); }

void join_cmd(Args a, std::string& out) {
    party::JoinOptions o;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == "script") o.source = party::Source::Script;
        else if (a[i] == "enemy") o.team = party::Team::Enemy;
        else if (a[i] == "friendly") o.team = party::Team::Friendly;
        else if (a[i] == "pad" && i + 1 < a.size()) o.pad = static_cast<int>(to_int(a[++i]));
        else if (a[i] == "slot" && i + 1 < a.size()) o.slot = to_int(a[++i]);
        else throw std::runtime_error("usage: elmp join [script] [pad N] [slot N] [friendly|enemy]");
    }
    std::int64_t slot = 0;
    must(party::join(o, &slot));
    line(out, std::format("joined {} ({}, {})", slot_label(slot), party::source_name(o.source), party::team_name(o.team)));
}

void leave_cmd(Args a, std::string& out) {
    std::int64_t slot = a.empty() ? 0 : to_int(a[0]);
    if (!slot) {
        const auto all = party::seats();
        if (all.empty()) return line(out, "no seats");
        slot = all.back().slot;
    }
    must(party::leave(slot));
    line(out, std::format("{} left", slot_label(slot)));
}

void seats_cmd(Args, std::string& out) {
    const auto all = party::seats();
    if (all.empty()) return line(out, "no seats");
    for (const auto& s : all) {
        const auto st = core::state(s.slot);
        const auto pad = input::pad::info(s.slot);
        std::string devs;
        for (const auto& [device, slot] : input::router::assignments())
            if (slot == s.slot) devs += " " + device;
        line(out, std::format("{}: {} {} alive={} hp={}/{} pos=({:.0f},{:.0f}) pad raw={} devices:{}", slot_label(s.slot), party::source_name(s.source),
                              party::team_name(s.team), st.alive, st.hp, st.max_hp, st.pos.y, st.pos.z, input::bits_text(pad.raw), devs.empty() ? " none" : devs));
    }
}

void devices_cmd(Args, std::string& out) {
    const auto table = input::router::assignments();
    int k = 0;
    for (const auto& d : input::rawinput::devices()) {
        const auto owner = input::router::owner(d.id);
        const auto it = table.find(d.id);
        line(out, std::format("{} {} [{}]{} -> {}{}", d.kind == "keyboard" ? std::format("kbd#{}", ++k) : std::string("pad   "), d.name, d.id,
                              d.connected ? "" : " (not connected)", slot_label(owner),
                              owner != 0 && it != table.end() && it->second != owner ? std::format(" (assigned {}, seat absent)", slot_label(it->second)) : ""));
    }
    line(out, std::format("wiring: window={} keyboard-raw={} xinput-iat={} active={} muted={} swallowed={}", input::rawinput::installed(),
                          input::rawinput::keyboard_registered(), input::xinput::installed(), input::router::active(), input::router::muted(),
                          input::router::swallowed()));
}

void assign_cmd(Args a, std::string& out) {
    std::size_t i = 0;
    const auto device = device_arg(a, i);
    if (i >= a.size()) throw std::runtime_error("usage: elmp assign <pad N|kbd#N|device-id> <slot|p1>");
    const std::int64_t target = a[i] == "p1" || a[i] == "P1" ? 1 : to_int(a[i]);
    input::router::assign(device, target);
    if (input::router::active()) party::devices::ensure_wiring();
    must(party::devices::save());
    line(out, std::format("{} -> {}", device, slot_label(target)));
}

void bindings_cmd(Args a, std::string& out) {
    if (a.empty()) throw std::runtime_error("usage: elmp bindings <slot> [reset]");
    const auto slot = to_int(a[0]);
    if (a.size() > 1 && a[1] == "reset") {
        input::router::set_bindings(slot, input::bindings::defaults());
        must(party::devices::save());
    }
    line(out, json::dump(input::bindings::to_json(input::router::bindings_of(slot)), 0));
}

// elmp bind <slot> <action> key|pad <label>[,<label>…]
void bind_cmd(Args a, std::string& out) {
    if (a.size() < 4) throw std::runtime_error("usage: elmp bind <slot> <action> key|pad <label[,label]>");
    const auto slot = to_int(a[0]);
    if (slot < 2) throw std::runtime_error("P1 uses the vanilla settings menu");
    const auto action = input::bindings::action_named(a[1]);
    if (!action) throw std::runtime_error("unknown action '" + a[1] + "'");
    const bool key = a[2] == "key";
    if (!key && a[2] != "pad") throw std::runtime_error("expected key or pad");
    std::vector<std::uint32_t> codes;
    std::size_t start = 0;
    const auto& spec = a[3];
    while (start <= spec.size()) {
        auto end = spec.find(',', start);
        if (end == std::string::npos) end = spec.size();
        const auto label = std::string_view(spec).substr(start, end - start);
        const auto code = key ? input::bindings::key_named(label) : input::bindings::pad_named(label);
        if (!code) throw std::runtime_error(std::format("unknown {} '{}'", key ? "key" : "pad control", label));
        codes.push_back(*code);
        start = end + 1;
    }
    auto b = input::router::bindings_of(slot);
    (key ? b.keys : b.pad)[*action] = codes;
    input::router::set_bindings(slot, b);
    must(party::devices::save());
    line(out, std::format("{} {} {} = {}", slot_label(slot), input::bindings::action_name(*action), a[2], spec));
}

void mute_cmd(Args a, std::string& out) {
    const bool on = a.empty() ? !input::router::muted() : (a[0] == "1" || a[0] == "on");
    must(settings::set("muteInput", on ? "1" : "0"));
    line(out, std::format("local input {}", on ? "muted (assignments kept)" : "unmuted"));
}

void progress_cmd(Args, std::string& out) { out += party::progress::describe(); }

void gates_cmd(Args, std::string& out) {
    out += core::permission::describe();
    out += party::rules::describe();
}

void perm_cmd(Args a, std::string& out) {
    if (a.size() < 3) throw std::runtime_error("usage: elmp perm <slot> <travel|rest|...> on|off");
    core::permission::Gate g;
    if (!core::permission::gate_named(a[1], g)) throw std::runtime_error("unknown gate " + a[1]);
    const auto slot = to_int(a[0]);
    if (slot < 2) throw std::runtime_error("P1 is not managed");
    core::permission::set(slot, g, a[2] == "on" || a[2] == "1");
    line(out, std::format("P{} {} = {} (effective {})", slot, a[1], a[2], core::permission::allows(slot, g) ? "on" : "off"));
}

void rest_cmd(Args a, std::string& out) {
    const auto slot = a.empty() ? (party::seats().empty() ? 0 : party::seats().front().slot) : to_int(a[0]);
    const auto why = party::bench::rest(slot);
    line(out, std::format("P{}: {}", slot, why.empty() ? "rested" : why));
}

void team_cmd(Args a, std::string& out) {
    if (a.size() < 2 || (a[1] != "friendly" && a[1] != "enemy")) throw std::runtime_error("usage: elmp team <slot> friendly|enemy");
    const auto slot = to_int(a[0]);
    must(party::set_team(slot, a[1] == "enemy" ? party::Team::Enemy : party::Team::Friendly));
    line(out, std::format("{} is {}", slot_label(slot), a[1]));
}

void pvp_cmd(Args, std::string& out) { out += pvp::describe(); }

void camera_cmd(Args, std::string& out) { out += party::camera_sync::describe(); }

// elmp menu <slot> [close | up|down|left|right|confirm|back|context|prev|next]
void menu_cmd(Args a, std::string& out) {
    if (a.empty()) return void(out += party::menu_ctrl::describe());
    if (a[0] == "close") {
        party::menu_ctrl::close();
        return line(out, "menu closed");
    }
    if (party::menu_ctrl::open_slot() == 0) {
        must(party::menu_ctrl::open(to_int(a[0])));
        if (a.size() == 1) return line(out, std::format("menu of P{} opened", a[0]));
    }
    for (std::size_t i = a.size() > 1 ? 1 : 0; i < a.size(); ++i) {
        if (a[i] == "close") party::menu_ctrl::close();
        else must(party::menu_ctrl::input(a[i]));
    }
    out += party::menu_ctrl::describe();
}

void manager_cmd(Args a, std::string& out) {
    if (!a.empty() && a[0] == "open") must(party::manager::open());
    else if (!a.empty() && a[0] == "close") party::manager::close();
    out += party::manager::describe();
}

void hud_cmd(Args, std::string& out) { out += party::hud_feed::describe(); }

void transit_cmd(Args, std::string& out) {
    out += party::transit::describe();
    out += party::respawn::describe();
}

#ifdef ELMP_DEV
// Test aids (write the world; register in reports): vanilla damage to a seat, teleport relative to P1.
void kill_cmd(Args a, std::string& out) {
    if (a.empty()) throw std::runtime_error("usage: elmp kill <slot> [damage]");
    const auto slot = to_int(a[0]);
    if (slot < 2) throw std::runtime_error("P1 is not a test target");
    const auto* s = core::schema::script();
    auto* p = core::pawn(slot);
    auto* hp = s && p ? s->zc_hp.object(p) : nullptr;
    if (!hp) throw std::runtime_error("no pawn");
    reflect::Resolver r("HPComponent", reflect::find_class(L"/Script/Zenith.HPComponent"));
    const auto f = r.func(L"DoDamage", 4);
    must(r.failure());
    const int dmg = a.size() > 1 ? static_cast<int>(to_int(a[1])) : 100000;
    reflect::Frame(f).arg(0, static_cast<ue::UObject*>(nullptr)).arg(1, dmg).arg_bool(2, false).arg_bool(3, false).invoke(hp);
    line(out, std::format("P{} took {} damage (vanilla DoDamage)", slot, dmg));
}

void tp_cmd(Args a, std::string& out) {
    if (a.size() < 3) throw std::runtime_error("usage: elmp tp <slot> <dy> <dz>  (relative to P1)");
    const auto slot = to_int(a[0]);
    auto* p1 = core::pawn(1);
    const auto base = p1 ? core::location(p1) : std::nullopt;
    if (!base) throw std::runtime_error("no P1 pawn");
    auto at = *base;
    at.y += static_cast<float>(std::stod(a[1]));
    at.z += static_cast<float>(std::stod(a[2]));
    must(party::respawn::teleport(slot, at));
    party::respawn::release(slot);
    line(out, std::format("P{} -> ({:.0f},{:.0f})", slot, at.y, at.z));
}

// elmp kbd <device> <key> down|up|tap
// elmp ui click|move <x> <y>   (Dev) feed the ELMP overlay directly; nothing reaches the window or the OS cursor
void ui_cmd(Args a, std::string& out) {
    if (a.size() < 3 || (a[0] != "click" && a[0] != "move")) throw std::runtime_error("usage: elmp ui click|move <x> <y>  (back-buffer pixels)");
    overlay::inject_mouse(static_cast<int>(to_int(a[1])), static_cast<int>(to_int(a[2])), a[0] == "click");
    line(out, std::format("overlay {} at ({},{})", a[0], a[1], a[2]));
}

void kbd_cmd(Args a, std::string& out) {
    std::size_t i = 0;
    const auto device = device_arg(a, i);
    if (i + 1 >= a.size()) throw std::runtime_error("usage: elmp kbd <device> <key> down|up|tap");
    const auto vk = input::bindings::key_named(a[i]);
    if (!vk) throw std::runtime_error("unknown key " + a[i]);
    must(party::devices::ensure_wiring());
    const auto& what = a[i + 1];
    if (what == "down" || what == "tap") must(input::rawinput::post_key(device, *vk, true));
    if (what == "up" || what == "tap") must(input::rawinput::post_key(device, *vk, false));
    line(out, std::format("{} {} {} (owner {})", device, a[i], what, slot_label(input::router::owner(device))));
}

// elmp fakepad <N> off | <buttons|-> [lx ly]   (buttons: A+X, lx/ly -1..1)
void fakepad_cmd(Args a, std::string& out) {
    if (a.size() < 2) throw std::runtime_error("usage: elmp fakepad <N> off | <buttons|-> [lx ly]");
    const int n = static_cast<int>(to_int(a[0]));
    if (a[1] == "off") {
        input::xinput::set_fake(n, std::nullopt);
        return line(out, std::format("fake pad {} removed", n));
    }
    input::gamepad::State s;
    if (a[1] != "-") {
        std::size_t start = 0;
        while (start <= a[1].size()) {
            auto end = a[1].find('+', start);
            if (end == std::string::npos) end = a[1].size();
            const auto c = input::bindings::pad_named(std::string_view(a[1]).substr(start, end - start));
            if (!c) throw std::runtime_error("unknown control " + a[1].substr(start, end - start));
            if (*c == input::bindings::kLeftTrigger) s.left_trigger = 255;
            else if (*c == input::bindings::kRightTrigger) s.right_trigger = 255;
            else s.buttons |= static_cast<std::uint16_t>(*c);
            start = end + 1;
        }
    }
    if (a.size() > 3) {
        s.thumb_lx = static_cast<std::int16_t>(std::stod(a[2]) * 32767);
        s.thumb_ly = static_cast<std::int16_t>(std::stod(a[3]) * 32767);
    }
    input::xinput::set_fake(n, s);
    line(out, std::format("fake pad {} buttons=0x{:04X} lx={} ly={} (owner {})", n, s.buttons, s.thumb_lx, s.thumb_ly, slot_label(input::router::owner(input::router::pad_id(n)))));
}
#endif

} // namespace

void register_party_commands() {
    add("join", "[script] [pad N] [slot N] [friendly|enemy]  add a local player behind P1", join_cmd);
    add("leave", "[slot]  remove a local player (equipment saved with save_scope, P1e)", leave_cmd);
    add("seats", "list local players", seats_cmd);
    add("devices", "keyboards / pads, effective owners, wiring state", devices_cmd);
    add("assign", "<pad N|kbd#N|device-id> <slot|p1>  (saved in devices.json)", assign_cmd);
    add("bind", "<slot> <action> key|pad <label[,label]>", bind_cmd);
    add("bindings", "<slot> [reset]", bindings_cmd);
    add("mute", "[0|1]  silence every physical input (not saved)", mute_cmd);
    add("progress", "shared progress: per-seat sync state, failures, grant log", progress_cmd);
    add("gates", "permission gates per seat, map rule 2 door state", gates_cmd);
    add("perm", "<slot> <gate> on|off  (travel rest trigger.story trigger.world pickup interact save)", perm_cmd);
    add("rest", "[slot]  PX bench rest (restore own HP / prayers / skill uses)", rest_cmd);
    add("transit", "map-follow state, last transit, revive / recall counters", transit_cmd);
    add("team", "<slot> friendly|enemy  (P1 is always friendly; opposing teams can hurt each other)", team_cmd);
    add("pvp", "hit windows: teams, damage / knockback rule, flip counters (rule via elmp set pvpDamage|pvpKnockback)", pvp_cmd);
    add("camera", "shared camera: framing hooks, ally targets, registrations", camera_cmd);
    add("menu", "[<slot> [inputs…] | close]  PX equipment menu (inputs: up down left right confirm back context prev next)", menu_cmd);
    add("manager", "[open|close]  ELMP manager window", manager_cmd);
    add("hud", "HUD feed: per-seat values, skill cache, anchors", hud_cmd);
#ifdef ELMP_DEV
    add("kill", "<slot> [damage]  (Dev aid) vanilla DoDamage on a seat", kill_cmd);
    add("tp", "<slot> <dy> <dz>  (Dev aid) teleport relative to P1", tp_cmd);
    add("kbd", "<device> <key> down|up|tap  (Dev) inject a keyboard event", kbd_cmd);
    add("ui", "click|move <x> <y>  (Dev) mouse event straight into the ELMP overlay (no window message)", ui_cmd);
    add("fakepad", "<N> off | <buttons|-> [lx ly]  (Dev) simulated XInput pad", fakepad_cmd);
#endif
}

} // namespace elmp::console
