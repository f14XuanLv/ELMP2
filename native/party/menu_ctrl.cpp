#include "party/menu_ctrl.h"

#include "party/bench.h"
#include "party/progress.h"
#include "party/seat.h"
#include "party/storage.h"
#include "party/transit.h"
#include "party/ui.h"

#include "core/catalog.h"
#include "core/player.h"
#include "core/profile.h"
#include "core/schema.h"
#include "core/vitals.h"

#include "input/bindings.h"
#include "input/router.h"
#include "input/xinput.h"

#include "overlay/model.h"

#include "engine/frame.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"

#include "base/clock.h"
#include "base/log.h"
#include "base/settings.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>

namespace elmp::party::menu_ctrl {

namespace b = input::bindings;

namespace {

enum class In { Up, Down, Left, Right, Confirm, Back, Context, Prev, Next };
constexpr std::array<int, 3> kCommands{3, 4, 5}; // ATTACK, SUMMON_1, SUMMON_2

constexpr const char* kOnlyAtRest = "Can only be changed at rest points";
constexpr const char* kRefusals[] = {"", "Insufficient slot count", "Passive limit reached", "Cannot equip"};

overlay::Menu g_m;
std::uint32_t g_prev = 0;
int g_dir = -1;
std::int64_t g_repeat_at = 0;
reflect::FuncRef g_used_slots;
bool g_used_tried = false;

void publish() { overlay::menu().store(g_m); }

void message(std::string_view text) {
    g_m.message.set(text);
    g_m.message_until = clock::now_s() + 2.5;
}

bool can_change() { return settings::get_bool("equipAnywhere") || bench::nearby(g_m.slot); }

std::string label(const b::Bindings& t, int action, bool pad) {
    const auto& list = pad ? t.pad[action] : t.keys[action];
    return list.empty() ? std::string("-") : pad ? b::pad_label(list[0]) : b::key_label(list[0]);
}

int used_slots(ue::UObject* pe) {
    if (!g_used_tried) {
        g_used_tried = true;
        reflect::Resolver r("PassiveEquipComponent", reflect::find_class(L"/Script/Zenith.PassiveEquipComponent"));
        g_used_slots = r.func(L"GetUsedSlotCount", 0);
    }
    if (!g_used_slots.ok() || !pe) return 0;
    reflect::Frame f(g_used_slots);
    return f.invoke(pe) ? f.ret<std::int32_t>() : 0;
}

// Data: owned spirits / relics and spirit levels of P1 (shared progress), equipment of the seat.
std::string rebuild() {
    std::string err;
    auto* pc = core::controller(g_m.slot);
    const auto mine = pc ? core::profile::read(pc, &err) : std::nullopt;
    const auto p1 = core::profile::read(core::controller(1), &err);
    if (!mine || !p1) return "profile read failed: " + err;
    g_m.can_change = can_change();
    g_m.reason.set(g_m.can_change ? "" : kOnlyAtRest);
    for (auto& set : g_m.sets)
        for (auto& c : set) c.set("");
    if (mine->spirits)
        for (const auto& s : *mine->spirits)
            for (int c = 0; c < 3; ++c)
                if (s.command == kCommands[c] && (s.set == 1 || s.set == 2)) g_m.sets[s.set - 1][c].set(s.id);
    auto owned = p1->spirit_items.value_or(std::vector<std::string>{});
    std::sort(owned.begin(), owned.end());
    g_m.nspirits = 0;
    for (const auto& id : owned) {
        if (g_m.nspirits >= overlay::kMaxSpirits) break;
        auto& it = g_m.spirits[g_m.nspirits++];
        it = {};
        const auto* c = core::catalog::spirit(id);
        it.id.set(id);
        it.name.set(c ? c->name : id);
        it.desc.set(c ? c->desc : "");
        it.icon.set(c ? c->icon : "spirits_" + id);
        it.main = c && c->main;
        int level = 1;
        if (p1->spirit_levels)
            if (const auto lv = p1->spirit_levels->find(id); lv != p1->spirit_levels->end()) level = lv->second;
        it.level = static_cast<std::int16_t>(level);
    }
    std::vector<std::string> relics = p1->passives && p1->passives->owned ? *p1->passives->owned : std::vector<std::string>{};
    std::sort(relics.begin(), relics.end());
    const auto equipped = mine->passives && mine->passives->equipped ? *mine->passives->equipped : std::vector<std::string>{};
    g_m.npassives = 0;
    for (const auto& id : relics) {
        if (g_m.npassives >= overlay::kMaxPassives) break;
        auto& it = g_m.passives[g_m.npassives++];
        it = {};
        const auto* c = core::catalog::passive(id);
        it.id.set(id);
        it.name.set(c ? c->name : id);
        it.desc.set(c ? c->desc : "");
        it.icon.set(c ? c->icon : "passives_" + id);
        it.slots = static_cast<std::int8_t>(c ? c->slots : 0);
        it.equipped = std::find(equipped.begin(), equipped.end(), id) != equipped.end();
    }
    const auto* s = core::schema::script();
    auto* pe = s && pc ? s->zpc_passive.object(pc) : nullptr;
    g_m.used_slots = used_slots(pe);
    g_m.max_slots = mine->passives && mine->passives->slots ? *mine->passives->slots : 0;
    const int n = g_m.page == 0 ? (g_m.in_list ? g_m.nspirits : 0) : g_m.npassives;
    g_m.cursor = std::clamp(g_m.cursor, 0, std::max(0, n - 1));
    return {};
}

void refresh() {
    if (auto e = rebuild(); !e.empty()) {
        log::warn("menu: {}", e);
        message("Cannot read equipment");
    }
    publish();
}

// v1 EquipItemToSlot semantics: replace the cell; the same spirit elsewhere in the set moves.
void equip_spirit(int set, int cell, std::string_view id) {
    auto* pc = core::controller(g_m.slot);
    std::string err;
    auto mine = pc ? core::profile::read(pc, &err) : std::nullopt;
    if (!mine || !mine->spirits) return message("Cannot equip");
    std::vector<core::profile::SpiritSlot> out;
    for (const auto& s : *mine->spirits)
        if (!(s.set == set + 1 && (s.command == kCommands[cell] || (!id.empty() && s.id == id)))) out.push_back(s);
    if (!id.empty()) out.push_back({set + 1, kCommands[cell], std::string(id)});
    core::profile::Profile p;
    p.spirits = out;
    if (const auto r = core::profile::apply(pc, p, true); !r.ok()) {
        log::warn("menu: P{} spirit change failed: {}", g_m.slot, r.text());
        message("Cannot equip");
    }
}

void toggle_passive(std::string_view id) {
    const auto* s = core::schema::script();
    auto* pc = core::controller(g_m.slot);
    auto* pe = s && pc ? s->zpc_passive.object(pc) : nullptr;
    if (!pe) return message("Cannot equip");
    const ue::FName name(log::wide(id).c_str(), RC::Unreal::FNAME_Add);
    reflect::Frame q(s->pe_is_equipped);
    q.arg_name(0, name);
    if (q.invoke(pe) && q.ret_bool()) {
        reflect::Frame(s->pe_unequip).arg_name(0, name).invoke(pe);
        return;
    }
    reflect::Frame e(s->pe_equip);
    e.arg_name(0, name);
    const int code = e.invoke(pe) ? e.ret<std::uint8_t>() : 3;
    if (code) message(kRefusals[std::clamp(code, 1, 3)]);
}

void handle(In e) {
    auto& v = g_m;
    const auto refuse = [&] { message(v.reason.view()); };
    if (e == In::Prev || e == In::Next) {
        v.page = v.page == 0 ? 1 : 0;
        v.in_list = false;
        v.cursor = 0;
        return publish();
    }
    if (v.page == 0 && !v.in_list) {
        const int linear = v.set * 3 + v.cell, max_linear = v.set2_locked ? 2 : 5;
        if (e == In::Left && linear > 0) v.set = (linear - 1) / 3, v.cell = (linear - 1) % 3;
        else if (e == In::Right && linear < max_linear) v.set = (linear + 1) / 3, v.cell = (linear + 1) % 3;
        else if (e == In::Up && v.set == 1) v.set = 0;
        else if (e == In::Down && v.set == 0 && !v.set2_locked) v.set = 1;
        else if (e == In::Confirm) {
            if (!v.can_change) refuse();
            else {
                v.in_list = true;
                v.cursor = 0;
                for (int i = 0; i < v.nspirits; ++i)
                    if (v.spirits[i].id.view() == v.sets[v.set][v.cell].view()) v.cursor = i;
            }
        } else if (e == In::Context) {
            if (!v.can_change) refuse();
            else if (!v.sets[v.set][v.cell].empty()) {
                equip_spirit(v.set, v.cell, {});
                return refresh();
            }
        } else if (e == In::Back) return close();
        return publish();
    }
    const int count = v.page == 0 ? v.nspirits : v.npassives;
    const auto move = [&](int d) {
        if (const int c = v.cursor + d; count > 0 && c >= 0 && c < count) v.cursor = c;
    };
    switch (e) {
    case In::Left: move(-1); break;
    case In::Right: move(1); break;
    case In::Up: move(-overlay::kMenuColumns); break;
    case In::Down: move(overlay::kMenuColumns); break;
    case In::Confirm:
        if (count == 0) break;
        if (!v.can_change) {
            refuse();
            break;
        }
        if (v.page == 0) {
            equip_spirit(v.set, v.cell, v.spirits[v.cursor].id.view());
            v.in_list = false;
        } else {
            toggle_passive(v.passives[v.cursor].id.view());
        }
        return refresh();
    case In::Back:
        if (v.page == 0) v.in_list = false;
        else return close();
        break;
    default: break;
    }
    publish();
}

void poll_devices(std::int64_t now) {
    const auto a = input::router::seat_actions(g_m.slot, &input::xinput::read, &b::menu_keys());
    const std::uint32_t pressed = a.held & ~g_prev;
    g_prev = a.held;
    const auto hit = [&](int action) { return (pressed >> action) & 1u; };
    if (hit(b::Jump)) handle(In::Confirm);
    if (!g_m.open) return;
    if (hit(b::Summon2) || hit(b::Menu)) handle(In::Back);
    if (!g_m.open) return;
    if (hit(b::Attack)) handle(In::Context);
    if (hit(b::Heal)) handle(In::Prev);
    if (hit(b::SummonSwitch)) handle(In::Next);
    int dir = -1;
    if (a.y > 0.6f) dir = 0;
    else if (a.y < -0.6f) dir = 1;
    else if (a.x < -0.6f) dir = 2;
    else if (a.x > 0.6f) dir = 3;
    if (dir < 0) {
        g_dir = -1;
        return;
    }
    if (dir != g_dir) {
        g_dir = dir;
        g_repeat_at = now + 350'000;
    } else if (now < g_repeat_at) {
        return;
    } else {
        g_repeat_at = now + 120'000;
    }
    static constexpr std::array<In, 4> map{In::Up, In::Down, In::Left, In::Right};
    handle(map[dir]);
}

void step(const frame::Tick& t) {
    if (!party::seat(g_m.slot) || !core::vitals::read(g_m.slot).alive || transit::active() || lifecycle::state() != lifecycle::State::Settled) return close();
    if (const bool can = can_change(); can != g_m.can_change) {
        g_m.can_change = can;
        g_m.reason.set(can ? "" : kOnlyAtRest);
        publish();
    }
    poll_devices(t.now_us);
}

void set_keys() {
    const bool pad = input::router::has_pad(g_m.slot);
    const auto& fixed = b::menu_keys();
    const auto own = input::router::bindings_of(g_m.slot);
    g_m.confirm.set(label(fixed, b::Jump, pad));
    g_m.back.set(label(fixed, b::Summon2, pad));
    g_m.context.set(label(fixed, b::Attack, pad));
    g_m.prev.set(label(fixed, b::Heal, pad));
    g_m.next.set(label(fixed, b::SummonSwitch, pad));
    for (int i = 0; i < 3; ++i) g_m.cells[i].set(label(own, std::array{b::Attack, b::Summon1, b::Summon2}[i], pad));
}

} // namespace

std::string open(std::int64_t slot) {
    if (g_m.open) return std::format("the menu of P{} is open", g_m.slot);
    if (!party::seat(slot)) return "not a local PX seat";
    if (transit::active() || lifecycle::state() != lifecycle::State::Settled) return "world is loading";
    if (!core::vitals::read(slot).alive) return "player is down";
    if (ui::pause_holders() & ui::Manager) return "close the ELMP manager first";
    if (auto e = progress::sync(slot); !e.empty()) {
        log::warn("menu: P{} shared progress sync failed: {}", slot, e);
        return "Shared progress sync failed";
    }
    g_m = {};
    g_m.open = true;
    g_m.slot = slot;
    set_keys();
    if (auto e = rebuild(); !e.empty()) {
        g_m = {};
        publish();
        return e;
    }
    g_prev = input::router::seat_actions(slot, &input::xinput::read, &b::menu_keys()).held; // the opening press does not count
    g_dir = -1;
    ui::hold_pause(ui::Menu, true);
    publish();
    log::info("menu: P{} opened", slot);
    return {};
}

void close() {
    if (!g_m.open) return;
    const auto slot = g_m.slot;
    g_m = {};
    publish();
    ui::hold_pause(ui::Menu, false);
    if (party::seat(slot)) storage::capture(slot, true, true); // the player's own equipment choice
    log::info("menu: P{} closed", slot);
}

std::int64_t open_slot() { return g_m.open ? g_m.slot : 0; }

std::string input(std::string_view what) {
    if (!g_m.open) return "menu is closed";
    static const std::map<std::string_view, In> names{{"up", In::Up},          {"down", In::Down},   {"left", In::Left},       {"right", In::Right}, {"confirm", In::Confirm},
                                                      {"back", In::Back},      {"context", In::Context}, {"prev", In::Prev}, {"next", In::Next}};
    const auto it = names.find(what);
    if (it == names.end()) return "expected up|down|left|right|confirm|back|context|prev|next";
    handle(it->second);
    return {};
}

std::string describe() {
    if (!g_m.open) return "menu closed\n";
    std::string sets;
    for (int s = 0; s < 2; ++s)
        for (int c = 0; c < 3; ++c) sets += std::format(" {}{}={}", s ? 'B' : 'A', c, g_m.sets[s][c].empty() ? "-" : g_m.sets[s][c].c_str());
    const auto& list = g_m.page == 0 ? g_m.spirits : g_m.passives;
    const int n = g_m.page == 0 ? g_m.nspirits : g_m.npassives;
    return std::format("P{} page={} in_list={} cell={}/{} cursor={} ({}) can_change={} spirits={} relics={} slots {}/{} message='{}'\nsets:{}\n", g_m.slot,
                       g_m.page ? "relics" : "spirits", g_m.in_list, g_m.set, g_m.cell, g_m.cursor, g_m.cursor < n ? list[g_m.cursor].id.c_str() : "-",
                       g_m.can_change, g_m.nspirits, g_m.npassives, g_m.used_slots, g_m.max_slots,
                       clock::now_s() < g_m.message_until ? g_m.message.c_str() : "", sets);
}

void install() {
    frame::add_phase("party.menu", step, [] { return g_m.open; });
    lifecycle::on_teardown([] {
        g_m = {};
        publish(); // the new world starts unpaused (ui drops its holders)
    });
}

} // namespace elmp::party::menu_ctrl
