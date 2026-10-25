#include "party/devices.h"
#include "party/menu_ctrl.h"

#include "input/bindings.h"
#include "input/rawinput.h"
#include "input/router.h"
#include "input/virtual_pad.h"
#include "input/xinput.h"

#include "engine/frame.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/json.h"
#include "base/log.h"
#include "base/settings.h"

#include <filesystem>
#include <format>
#include <map>

namespace elmp::party::devices {

namespace {

constexpr int kSchema = 1;
std::set<std::int64_t> g_seats;
std::map<std::int64_t, std::uint32_t> g_ui_presses;
bool g_loaded = false;

std::filesystem::path file() {
    const auto dir = world::saved_dir();
    return dir.empty() ? std::filesystem::path{} : std::filesystem::path(dir) / L"ELMP" / L"ELMP" / L"devices.json";
}

void phase(const frame::Tick& t) { pump(t.now_us); }

} // namespace

std::string load() {
    const auto f = file();
    if (f.empty()) return "saved dir unknown";
    g_loaded = true;
    std::string err;
    auto root = json_io::load(f, kSchema, &err);
    if (!root) return err == "missing" ? std::string{} : err;
    std::map<std::string, std::int64_t> table;
    if (const auto* a = root->get("assignments"); a && a->is_object())
        for (const auto& [device, slot] : a->obj)
            if (slot.is_number()) table[device] = static_cast<std::int64_t>(slot.num);
    input::router::set_assignments(std::move(table));
    if (const auto* b = root->get("bindings"); b && b->is_object())
        for (const auto& [slot, v] : b->obj) input::router::set_bindings(std::stoll(slot), input::bindings::from_json(v, input::bindings::defaults()));
    return {};
}

std::string save() {
    const auto f = file();
    if (f.empty()) return "saved dir unknown";
    auto root = json::Value::object();
    root.set("schema", kSchema);
    auto a = json::Value::object();
    for (const auto& [device, slot] : input::router::assignments()) a.set(device, slot);
    root.set("assignments", std::move(a));
    auto b = json::Value::object();
    for (const auto& [slot, bind] : input::router::all_bindings()) b.set(std::to_string(slot), input::bindings::to_json(bind));
    root.set("bindings", std::move(b));
    std::string err;
    if (!json_io::save(f, root, &err)) return err;
    return {};
}

std::string ensure_wiring() {
    std::string err;
    if (auto e = input::rawinput::install(); !e.empty()) err = "rawinput: " + e;
    if (auto e = input::xinput::install(); !e.empty()) err += (err.empty() ? "" : "; ") + ("xinput: " + e);
    input::xinput::refresh_connected();
    if (!err.empty()) log::warn("input wiring: {}", err);
    return err;
}

void sync_seats(const std::set<std::int64_t>& seats) {
    g_seats = seats;
    std::erase_if(g_ui_presses, [&](const auto& e) { return !seats.contains(e.first); });
    input::router::set_seats(seats);
    if (input::router::active()) ensure_wiring();
}

std::string auto_assign(std::int64_t slot, int preferred) {
    if (preferred >= 0) {
        if (preferred >= input::router::kPads) return "pad index must be 0..3";
        input::router::assign(input::router::pad_id(preferred), slot);
        return save();
    }
    for (const auto& [device, s] : input::router::assignments())
        if (s == slot) return {}; // already has devices
    input::xinput::refresh_connected();
    const auto table = input::router::assignments();
    for (int i = 0; i < input::router::kPads; ++i) {
        const auto id = input::router::pad_id(i);
        if (input::router::connected(i) && !table.contains(id)) {
            input::router::assign(id, slot);
            return save();
        }
    }
    return {}; // no free connected pad: the seat waits for an assignment (or is script-driven)
}

void pump(std::int64_t now_us) {
    (void)now_us;
    for (const auto slot : g_seats) {
        if (!input::router::has_devices(slot)) continue;
        if (slot == menu_ctrl::open_slot()) { // the menu reads this seat's devices; the character gets nothing
            input::pad::set(slot, 0, 0, 0);
            continue;
        }
        const auto a = input::router::seat_actions(slot, &input::xinput::read);
        g_ui_presses[slot] |= input::router::pressed(slot, a) & ((1u << input::bindings::Menu) | (1u << input::bindings::HudToggle));
        input::pad::set(slot, a.x, a.y, input::bindings::command_bits(a));
    }
}

bool pumping() { return !g_seats.empty(); }

std::uint32_t take_ui_presses(std::int64_t slot) {
    auto it = g_ui_presses.find(slot);
    if (it == g_ui_presses.end()) return 0;
    const auto v = it->second;
    it->second = 0;
    return v;
}

void install() {
    frame::add_phase("input.pump", phase, [] { return pumping(); });
    frame::add_phase("devices.load", [](const frame::Tick&) {
        if (auto err = load(); !err.empty() && err != "saved dir unknown") log::warn("devices.json: {}", err);
    }, [] { return !g_loaded; }, 500);
    settings::on_change([](const std::string& key) {
        if (key != "muteInput") return;
        const bool m = settings::get_bool("muteInput");
        input::router::set_muted(m);
        if (m) ensure_wiring();
        log::info("local input {}", m ? "muted" : "unmuted");
    });
}

} // namespace elmp::party::devices
