#include "party/hud_feed.h"

#include "party/bench.h"
#include "party/devices.h"
#include "party/manager.h"
#include "party/menu_ctrl.h"
#include "party/seat.h"
#include "party/transit.h"
#include "party/ui.h"

#include "core/catalog.h"
#include "core/player.h"
#include "core/schema.h"
#include "core/vitals.h"

#include "input/bindings.h"
#include "input/router.h"

#include "overlay/model.h"

#include "engine/frame.h"
#include "engine/handle.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/events.h"
#include "base/log.h"
#include "base/metrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <unordered_map>

namespace elmp::party::hud_feed {

using reflect::Kind;
namespace b = input::bindings;

namespace {

constexpr float kHeadOffset = 115.0f;
constexpr std::array<std::uint8_t, 3> kCommands{3, 4, 5}; // ECommandInputTypes ATTACK, SUMMON_1, SUMMON_2
constexpr std::array<int, 3> kActionOf{b::Attack, b::Summon1, b::Summon2};

struct Schema {
    reflect::PropRef spirit_settings, settings_struct, action_type;
    reflect::FuncRef get_or_create;
};
std::optional<Schema> g_schema;
std::string g_failure;

struct ActionFns {
    reflect::FuncRef remaining, limit, cooldown, recast;
    bool cast_limit = false;
};
std::unordered_map<ObjHandle, ActionFns> g_fns; // per action class

struct Skill {
    ue::UClass* type = nullptr;
    ObjHandle action;
    const ActionFns* fns = nullptr;
    std::int8_t limit = -1;
    float recast = 0;
};
struct SeatState {
    ObjHandle pawn;
    std::uint8_t set = 0;
    std::array<Skill, 3> skills{};
    std::int64_t next_slow_us = 0;
    overlay::HudSeat model;
};
std::map<std::int64_t, SeatState> g_seats;
overlay::Hud g_staging;
bool g_published = false;
std::uint64_t g_events = 0;
metrics::Counter& c_rebuilds = metrics::counter("hud.skill_rebuilds");

const Schema* schema() {
    if (g_schema || !g_failure.empty()) return g_schema ? &*g_schema : nullptr;
    Schema s;
    reflect::Resolver sd("SpiritData", static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.SpiritData")));
    s.spirit_settings = sd.prop(L"CommandSettingsData", Kind::Object);
    reflect::Resolver csd("CommandSettingsData", reflect::find_class(L"/Script/Zenith.CommandSettingsData"));
    s.settings_struct = csd.prop(L"CommandSettings", Kind::Struct);
    reflect::Resolver cs("CommandSettings", static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.CommandSettings")));
    s.action_type = cs.prop(L"CommandActionType", Kind::Class);
    reflect::Resolver cq("CommandQueueComponent", reflect::find_class(L"/Script/Zenith.CommandQueueComponent"));
    s.get_or_create = cq.func(L"GetOrCreateCommandAction", 1);
    for (const auto* r : {&sd, &csd, &cs, &cq})
        if (!r->ok() && g_failure.empty()) g_failure = r->failure();
    if (!g_failure.empty()) {
        log::error("HUD skills unavailable: {}", g_failure);
        return nullptr;
    }
    g_schema = s;
    return &*g_schema;
}

const ActionFns& fns_of(ue::UClass* cls) {
    auto& f = g_fns[ObjHandle::of(cls)];
    if (f.recast.ok()) return f;
    reflect::Resolver r("CommandAction", cls);
    f.recast = r.func(L"GetRecastTime", 0);
    f.cooldown = r.func(L"GetCooldownTime", 0);
    reflect::Resolver l("CastLimit", cls); // CommandActionSummon / CommandActionComboSummon implement the cast-limit interface
    f.remaining = l.func(L"GetRemainingCastValue", 0);
    f.limit = l.func(L"GetCastLimitType", 0);
    f.cast_limit = l.ok();
    return f;
}

template <class T>
T call(const reflect::FuncRef& f, ue::UObject* o, T fallback) {
    if (!f.ok() || !o) return fallback;
    reflect::Frame fr(f);
    return fr.invoke(o) ? fr.ret<T>() : fallback;
}

// The three equipped action classes of the current summon set (direct reads of SummonSets).
std::array<ue::UClass*, 3> equipped(const core::schema::Script& cs, const Schema& s, ue::UObject* equip, std::uint8_t set, std::array<std::string, 3>& ids) {
    std::array<ue::UClass*, 3> out{};
    for (const auto& st : reflect::map_pairs(equip, cs.se_sets)) {
        if (*st.key != set) continue;
        for (const auto& sp : reflect::map_pairs(st.value, cs.set_spirits)) {
            for (int i = 0; i < 3; ++i) {
                if (*sp.key != kCommands[i]) continue;
                ids[i] = ue::utf8(cs.spirit_id.read<ue::FName>(sp.value));
                auto* data = s.spirit_settings.read<ue::UObject*>(sp.value);
                if (ue::alive(data)) out[i] = s.action_type.read<ue::UClass*>(s.settings_struct.at<std::uint8_t>(data)); // asset.CommandSettings.CommandActionType
            }
        }
    }
    return out;
}

std::string key_label(std::int64_t slot, int action) {
    const auto* seat = party::seat(slot);
    if (!seat || seat->source == Source::Script) return action == b::Attack ? "A1" : action == b::Summon1 ? "S1" : action == b::Summon2 ? "S2" : "Interact";
    const auto bind = input::router::bindings_of(slot);
    const bool pad = input::router::has_pad(slot);
    const auto& list = pad ? bind.pad[action] : bind.keys[action];
    return list.empty() ? std::string("-") : pad ? b::pad_label(list[0]) : b::key_label(list[0]);
}

void rebuild(std::int64_t slot, SeatState& st, ue::UObject* pawn, const std::array<ue::UClass*, 3>& types, const std::array<std::string, 3>& ids) {
    const auto* cs = core::schema::script();
    const auto* s = schema();
    auto* queue = cs && pawn ? cs->zc_command_queue.object(pawn) : nullptr;
    ++c_rebuilds;
    for (int i = 0; i < 3; ++i) {
        auto& k = st.skills[i];
        k = Skill{types[i]};
        auto& m = st.model.skills[i];
        m = {};
        m.key.set(key_label(slot, kActionOf[i]));
        if (ids[i].empty() || ids[i] == "None") continue;
        const auto* sp = core::catalog::spirit(ids[i]);
        m.icon.set(sp ? sp->icon : "spirits_" + ids[i]);
        if (!types[i] || !queue || !s) continue;
        reflect::Frame f(s->get_or_create);
        f.arg(0, types[i]);
        auto* action = f.invoke(queue) ? f.ret<ue::UObject*>() : nullptr;
        if (!ue::alive(action)) continue;
        k.action = ObjHandle::of(action);
        k.fns = &fns_of(action->GetClassPrivate());
        k.limit = k.fns->cast_limit ? static_cast<std::int8_t>(call<std::uint8_t>(k.fns->limit, action, 0)) : 0;
        if (k.limit > 3) k.limit = -1;
        k.recast = call<float>(k.fns->recast, action, 0.0f);
    }
}

void slow(std::int64_t slot, SeatState& st, ue::UObject* pc, ue::UObject* pawn) {
    const auto* cs = core::schema::script();
    const auto* s = schema();
    auto& m = st.model;
    const auto v = core::vitals::read(slot);
    m.alive = v.alive;
    m.hp = static_cast<float>(v.hp), m.max_hp = static_cast<float>(v.max_hp), m.mp = static_cast<float>(v.mp), m.max_mp = static_cast<float>(v.max_mp);
    m.heals = v.heals, m.max_heals = v.max_heals;
    m.special_cost = ui::special_cost();
    if (cs && pc) {
        reflect::Frame f(cs->zpc_aptitude_unlocked);
        f.arg(0, core::vitals::kAptitudeSpecialAttack);
        m.special_unlocked = f.invoke(pc) && f.ret_bool();
    }
    const auto* seat = party::seat(slot);
    m.team = seat && seat->team == Team::Enemy ? 2 : 1;
    m.prompt.set(v.alive && bench::nearby(slot) ? key_label(slot, b::Interact) + "  Rest" : std::string());
    auto* equip = cs && pc ? cs->zpc_spirit_equip.object(pc) : nullptr;
    if (!equip || !s) return;
    m.summon_set = st.set = call<std::uint8_t>(cs->se_current, equip, 1);
    std::array<std::string, 3> ids;
    const auto types = equipped(*cs, *s, equip, st.set, ids);
    bool same = st.pawn.get() == pawn && pawn;
    for (int i = 0; i < 3 && same; ++i) same = st.skills[i].type == types[i] && (types[i] == nullptr || st.skills[i].action.valid());
    if (!same) {
        st.pawn = ObjHandle::of(pawn);
        rebuild(slot, st, pawn, types, ids);
    } else {
        for (int i = 0; i < 3; ++i) {
            m.skills[i].key.set(key_label(slot, kActionOf[i]));
            if (auto* a = st.skills[i].action.get(); a && st.skills[i].fns) st.skills[i].recast = call<float>(st.skills[i].fns->recast, a, 0.0f); // relics change it
        }
    }
}

void fast(SeatState& st, ue::UObject* pawn) {
    auto& m = st.model;
    m.anchored = false;
    if (auto at = pawn ? core::location(pawn) : std::nullopt) {
        at->z += kHeadOffset;
        if (const auto scr = ui::project(*at)) m.anchored = true, m.ax = scr->x, m.ay = scr->y;
    }
    for (int i = 0; i < 3; ++i) {
        const auto& k = st.skills[i];
        auto& out = m.skills[i];
        out.limit = k.limit;
        auto* a = k.action.get();
        if (!a || !k.fns) {
            out.limit = -1;
            continue;
        }
        if (k.limit > 0) out.remaining = std::max(0, static_cast<int>(std::ceil(call<float>(k.fns->remaining, a, 0.0f))));
        out.cooldown = k.recast > 0 ? std::clamp(call<float>(k.fns->cooldown, a, 0.0f) / k.recast, 0.0f, 1.0f) : 0.0f;
    }
}

void consume_events() {
    std::array<events::Event, 32> buf;
    std::size_t n;
    while ((n = events::bus().since(g_events, buf)) > 0)
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = buf[i];
            if (e.type == events::Type::Revived) toast(e.slot, "Revived");
            else if (e.type == events::Type::Rested) toast(e.slot, "Rested");
            else if (e.type == events::Type::Recalled) toast(e.slot, "Recalled");
        }
}

void step(const frame::Tick& t) {
    const auto all = party::seats();
    std::erase_if(g_seats, [&](const auto& e) { return std::none_of(all.begin(), all.end(), [&](const Seat& s) { return s.slot == e.first; }); });
    if (all.empty()) {
        g_staging = {};
        overlay::hud().store(g_staging);
        g_published = false;
        return;
    }
    consume_events();
    bool slow_done = false; // at most one seat's 250 ms refresh per frame: no aligned spikes with many seats
    for (const auto& seat : all) {
        auto& st = g_seats[seat.slot];
        st.model.slot = seat.slot;
        const auto presses = devices::take_ui_presses(seat.slot);
        if (presses & (1u << b::HudToggle)) st.model.collapsed = !st.model.collapsed;
        if ((presses & (1u << b::Menu)) && menu_ctrl::open_slot() == 0)
            if (auto e = menu_ctrl::open(seat.slot); !e.empty()) toast(seat.slot, e, 2.5);
    }
    const bool visible = !transit::active() && !world::paused() && menu_ctrl::open_slot() == 0 && !manager::is_open() && !ui::vanilla_menu_open() && ui::display_player_ui();
    g_staging.visible = visible;
    g_staging.count = 0;
    for (const auto& seat : all) {
        auto& st = g_seats[seat.slot];
        auto* pc = core::controller(seat.slot);
        auto* pawn = core::pawn(seat.slot);
        if (st.model.max_hp == 0 || (!slow_done && t.now_us >= st.next_slow_us)) {
            slow(seat.slot, st, pc, pawn);
            st.next_slow_us = t.now_us + 250'000;
            slow_done = true;
        }
        if (visible) fast(st, pawn);
        if (g_staging.count < overlay::kMaxSeats) g_staging.seats[g_staging.count++] = st.model;
    }
    overlay::hud().store(g_staging);
    g_published = true;
}

} // namespace

void toast(std::int64_t slot, std::string_view text, double seconds) {
    auto it = g_seats.find(slot);
    if (it == g_seats.end()) return;
    it->second.model.toast.set(text);
    it->second.model.toast_until = clock::now_s() + seconds;
}

void forget(std::int64_t slot) { g_seats.erase(slot); }

std::string describe() {
    std::string out = std::format("published={} skill_rebuilds={} schema={} {}\n", g_published, c_rebuilds.load(), g_failure.empty() ? "ok" : g_failure, ui::projection_status());
    for (const auto& [slot, st] : g_seats) {
        const auto& m = st.model;
        out += std::format("P{}: hp {:.0f}/{:.0f} mp {:.0f}/{:.0f} heals {}/{} set {} special={} anchor={} ({:.0f},{:.0f}) collapsed={} prompt='{}'\n", slot, m.hp, m.max_hp, m.mp,
                           m.max_mp, m.heals, m.max_heals, m.summon_set, m.special_unlocked, m.anchored, m.ax, m.ay, m.collapsed, m.prompt.c_str());
        for (int i = 0; i < 3; ++i) {
            const auto& k = m.skills[i];
            out += std::format("   [{}] {} icon={} limit={} remaining={} cooldown={:.2f} recast={:.2f}\n", i, k.key.c_str(), k.icon.empty() ? "-" : k.icon.c_str(), k.limit,
                               k.remaining, k.cooldown, st.skills[i].recast);
        }
    }
    return out;
}

void install() {
    g_events = events::bus().last_seq();
    frame::add_phase("party.hud", step, [] { return party::count() > 0 || g_published; });
    lifecycle::on_teardown([] {
        g_seats.clear();
        g_fns.clear();
        g_staging = {};
        overlay::hud().store(g_staging);
        g_events = events::bus().last_seq();
    });
}

} // namespace elmp::party::hud_feed
