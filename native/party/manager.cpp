#include "party/manager.h"

#include "party/devices.h"
#include "party/menu_ctrl.h"
#include "party/seat.h"
#include "party/storage.h"
#include "party/transit.h"
#include "party/ui.h"

#include "core/player.h"
#include "core/skin.h"
#include "core/vitals.h"

#include "input/bindings.h"
#include "input/rawinput.h"
#include "input/router.h"
#include "input/xinput.h"

#include "overlay/model.h"
#include "overlay/render.h"

#include "engine/frame.h"
#include "engine/lifecycle.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/log.h"
#include "base/settings.h"

#include <cstdlib>
#include <format>
#include <vector>

namespace elmp::party::manager {

namespace b = input::bindings;

namespace {

overlay::Manager g_m;
bool g_dirty = true;
std::vector<input::rawinput::Device> g_devices;
clock::Cadence g_refresh(100);
std::int64_t g_capture_until = 0;
std::uint32_t g_pad_prev[input::router::kPads]{};

void message(std::string_view text) {
    g_m.message.set(text);
    g_m.message_until = clock::now_s() + 3.0;
    g_dirty = true;
    log::info("manager: {}", text);
}

std::uint32_t pad_controls(int i) {
    if (!input::router::connected(i)) return 0;
    const auto s = input::xinput::read(i);
    if (!s) return 0;
    std::uint32_t c = s->buttons;
    if (s->left_trigger > input::gamepad::kTriggerThreshold) c |= b::kLeftTrigger;
    if (s->right_trigger > input::gamepad::kTriggerThreshold) c |= b::kRightTrigger;
    return c;
}

bool pad_active(int i) {
    if (!input::router::connected(i)) return false;
    const auto s = input::xinput::read(i);
    return s && (s->buttons || s->left_trigger > 30 || s->right_trigger > 30 || std::abs(s->thumb_lx) > 12000 || std::abs(s->thumb_ly) > 12000);
}

std::string labels(const std::vector<std::uint32_t>& list, bool keyboard) {
    std::string out;
    for (const auto v : list) out += (out.empty() ? "" : " / ") + (keyboard ? b::key_label(v) : b::pad_label(v));
    return out;
}

void fill() {
    g_m.muted = input::router::muted();
    const auto seats = party::seats();
    g_m.nseats = 0;
    for (const auto& s : seats) {
        if (g_m.nseats >= overlay::kMaxSeats) break;
        auto& m = g_m.seats[g_m.nseats++];
        m = {};
        m.slot = s.slot;
        m.alive = core::vitals::read(s.slot).alive;
        const int skin = core::skin::current(core::pawn(s.slot));
        m.skin = core::skin::valid(skin) ? skin : core::skin::kDefault;
        m.team = s.team == Team::Enemy ? 2 : 1;
        const auto bind = input::router::bindings_of(s.slot);
        for (int a = 0; a < overlay::kActions; ++a) {
            m.keys[a].set(labels(bind.keys[a], true));
            m.pads[a].set(labels(bind.pad[a], false));
        }
    }
    const auto table = input::router::assignments();
    g_m.ndevices = 0;
    for (const auto& d : g_devices) {
        if (g_m.ndevices >= static_cast<int>(std::size(g_m.devices))) break;
        auto& m = g_m.devices[g_m.ndevices++];
        m = {};
        m.id.set(d.id);
        m.name.set(d.name);
        m.keyboard = d.kind == "keyboard";
        const auto pad = input::router::pad_index(d.id);
        m.connected = pad ? input::router::connected(*pad) : d.connected;
        m.active = pad ? pad_active(*pad) : input::router::any_key(d.id);
        m.assigned = input::router::owner(d.id); // effective owner: a device saved for an absent seat controls P1
    }
    auto& r = g_m.rules;
    r.map_rule = static_cast<int>(settings::get_number("mapRule"));
    r.pvp_damage_pct = static_cast<int>(settings::get_number("pvpDamage") * 100 + 0.5);
    r.knockback = static_cast<int>(settings::get_number("pvpKnockback"));
    r.equip_anywhere = settings::get_bool("equipAnywhere");
    r.recall = settings::get_bool("recall");
    r.camera_framing = settings::get_bool("cameraFraming");
    r.recall_distance = static_cast<int>(settings::get_number("recallDistance"));
    r.recall_delay_ms = static_cast<int>(settings::get_number("recallDelayMs"));
    r.recall_fall_depth = static_cast<int>(settings::get_number("recallFallDepth"));
    const auto sv = storage::view();
    g_m.storage_id.set(sv.id);
    g_m.storage_message.set(sv.message);
    g_m.storage_blocked = sv.blocked;
    g_m.storage_can_reuse = sv.can_reuse;
    g_dirty = true;
}

void end_capture() {
    g_m.capturing = false;
    g_dirty = true;
}

void finish_capture(std::uint32_t value) {
    auto bind = input::router::bindings_of(g_m.capture_slot);
    (g_m.capture_keyboard ? bind.keys : bind.pad)[g_m.capture_action] = {value};
    input::router::set_bindings(g_m.capture_slot, bind);
    if (auto e = devices::save(); !e.empty()) message("Bindings not saved: " + e);
    end_capture();
    fill();
}

void poll_capture(std::int64_t now) {
    if (!g_m.capturing) return;
    if (now > g_capture_until) return end_capture();
    if (g_m.capture_keyboard) {
        if (const auto vk = input::rawinput::take_captured_key()) finish_capture(vk);
        return;
    }
    for (int i = 0; i < input::router::kPads; ++i) {
        const auto c = pad_controls(i), pressed = c & ~g_pad_prev[i];
        g_pad_prev[i] = c;
        if (pressed) return finish_capture(pressed & (~pressed + 1)); // lowest new control
    }
}

void rule(std::string_view key, double value) {
    std::string err;
    for (const auto& e : settings::all()) {
        if (e.spec.key != key) continue;
        err = std::holds_alternative<bool>(e.spec.def) ? settings::set_value(key, value != 0) : settings::set_value(key, value);
        if (!err.empty()) message(std::format("{}: {}", key, err));
        fill();
        return;
    }
    message(std::format("unknown rule {}", key));
}

void handle(const overlay::Request& r) {
    using overlay::Req;
    switch (r.kind) {
    case Req::ManagerOpen:
        if (auto e = open(); !e.empty()) log::info("manager: {}", e);
        break;
    case Req::ManagerClose: close(); break;
    case Req::AddPlayer: {
        std::int64_t slot = 0;
        if (auto e = party::join({}, &slot); !e.empty()) message("Cannot add a player: " + e);
        else message(std::format("P{} joined", slot));
        fill();
        break;
    }
    case Req::RemovePlayer:
        if (auto e = party::leave(r.slot); !e.empty()) message("Cannot remove the player: " + e);
        fill();
        break;
    case Req::Skin:
        if (auto e = core::skin::apply(core::pawn(r.slot), static_cast<int>(r.value)); !e.empty()) message("Cannot change the skin: " + e);
        else storage::capture(r.slot, true);
        fill();
        break;
    case Req::Team:
        if (auto e = party::set_team(r.slot, r.value == 2 ? Team::Enemy : Team::Friendly); !e.empty()) message("Cannot change the side: " + e);
        fill();
        break;
    case Req::Assign:
        input::router::assign(std::string(r.text.view()), r.slot);
        if (input::router::active()) devices::ensure_wiring();
        if (auto e = devices::save(); !e.empty()) message("Assignment not saved: " + e);
        fill();
        break;
    case Req::Mute:
        settings::set("muteInput", r.value != 0 ? "1" : "0");
        fill();
        break;
    case Req::Capture:
        g_m.capturing = true;
        g_m.capture_slot = r.slot;
        g_m.capture_keyboard = r.keyboard;
        g_m.capture_action = r.action;
        g_capture_until = clock::now_us() + 8'000'000;
        if (r.keyboard) input::rawinput::begin_capture();
        else
            for (int i = 0; i < input::router::kPads; ++i) g_pad_prev[i] = pad_controls(i); // buttons already held do not count
        g_dirty = true;
        break;
    case Req::CancelCapture: end_capture(); break;
    case Req::ResetBindings:
        input::router::set_bindings(r.slot, b::defaults());
        if (auto e = devices::save(); !e.empty()) message("Bindings not saved: " + e);
        fill();
        break;
    case Req::Rule: rule(r.text.view(), r.value); break;
    case Req::StorageNew:
    case Req::StorageReuse:
        if (auto e = storage::choose(r.kind == Req::StorageNew); !e.empty()) message("PX data: " + e);
        else message(r.kind == Req::StorageNew ? "New PX data started for this save" : "Previous PX data reused");
        fill();
        break;
    }
}

bool button() {
    if (g_m.open || !overlay::ready() || lifecycle::state() != lifecycle::State::Settled || !world::p1_pawn() || world::paused() || transit::active() ||
        menu_ctrl::open_slot())
        return false;
    return party::count() == 0 || !ui::vanilla_menu_open();
}

void step(const frame::Tick& t) {
    overlay::Request r;
    for (int i = 0; i < 8 && overlay::next_request(r); ++i) handle(r);
    if (g_m.open) {
        if (lifecycle::state() != lifecycle::State::Settled || transit::active()) close();
        else {
            poll_capture(t.now_us);
            if (g_refresh.due(t.now_us)) fill();
        }
    }
    if (const bool v = button(); v != g_m.button) g_m.button = v, g_dirty = true;
    if (g_dirty) {
        overlay::manager().store(g_m);
        g_dirty = false;
    }
}

} // namespace

std::string open() {
    if (g_m.open) return {};
    if (lifecycle::state() != lifecycle::State::Settled || !world::p1_pawn()) return "not in a game world";
    if (menu_ctrl::open_slot()) return "a PX equipment menu is open";
    g_devices = input::rawinput::devices();
    input::xinput::refresh_connected();
    input::rawinput::set_observe(true);
    g_m.open = true;
    ui::hold_pause(ui::Manager, true);
    fill();
    log::info("manager opened");
    return {};
}

void close() {
    if (!g_m.open) return;
    g_m.open = false;
    end_capture();
    input::rawinput::set_observe(false);
    ui::hold_pause(ui::Manager, false);
    g_dirty = true;
    log::info("manager closed");
}

bool is_open() { return g_m.open; }

std::string describe() {
    return std::format("open={} button={} seats={} devices={} capturing={} (P{} {} action {})\noverlay: {}\n", g_m.open, g_m.button, g_m.nseats, g_m.ndevices,
                       g_m.capturing, g_m.capture_slot, g_m.capture_keyboard ? "key" : "pad", g_m.capture_action, overlay::debug());
}

void install() {
    frame::add_phase("party.manager", step);
    lifecycle::on_teardown([] {
        if (g_m.open) input::rawinput::set_observe(false);
        g_m.open = false;
        g_m.capturing = false;
        g_m.nseats = 0; // seats are dropped with the world
        g_dirty = true;
    });
}

} // namespace elmp::party::manager
