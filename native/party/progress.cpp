#include "party/progress.h"

#include "party/seat.h"
#include "party/transit.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/frame.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/events.h"
#include "base/log.h"
#include "base/metrics.h"

#include <Unreal/Engine/UDataTable.hpp>

#include <array>
#include <bitset>
#include <cstring>
#include <deque>
#include <format>
#include <map>

namespace elmp::party::progress {

using namespace RC::Unreal;

namespace {

constexpr std::array<const char*, 4> kKinds{"aptitude", "parameter", "passive", "spirit"};
constexpr int kCurrencies = 3; // ECurrencyType Spirit_Lv1..3
constexpr std::uint8_t kSpecialAttack = 120;

struct Snap {
    bool ok = false;
    int level = 0;
    std::int64_t exp_total = 0;
    std::array<std::int64_t, kCurrencies> currency{};
    std::array<std::map<std::uint32_t, int>, 4> counts;
    std::bitset<256> unlocked;
    std::map<std::uint32_t, int> spirit_levels; // > 1 only (level 1 = default, normalised away)
    std::array<int, 3> heal{};
    int slots = 0;
};

struct SeatState {
    bool has_baseline = false;
    Snap baseline;
    std::uint64_t synced_host = 0, synced_seat = 0;
    std::string failure;
};

std::map<std::int64_t, SeatState> g_seats;
std::map<std::uint32_t, FName> g_names; // name key → FName (for granting rows)
std::vector<int> g_needs;               // index level-1 → exp needed for the next level
ObjHandle g_needs_for;
std::deque<std::string> g_log;
metrics::Counter& c_mirrored = metrics::counter("progress.mirrored");
metrics::Counter& c_grants = metrics::counter("progress.grants");
metrics::Counter& c_failures = metrics::counter("progress.failures");

int call_int(const reflect::FuncRef& f, ue::UObject* self) {
    reflect::Frame fr(f);
    return fr.invoke(self) ? fr.ret<std::int32_t>() : 0;
}

const std::vector<int>& level_needs(const core::schema::Script& s) {
    auto* gm = s.world_game_mode.object(world::world());
    auto* table = gm ? static_cast<UDataTable*>(s.gm_level_table.object(gm)) : nullptr;
    if (!table || (g_needs_for.get() == table && !g_needs.empty())) return g_needs;
    g_needs.clear();
    g_needs_for = ObjHandle::of(table);
    UScriptStruct* rs = table->GetRowStruct();
    auto* need = rs ? reflect::find_property(rs, L"NecessaryExperiencePointsForLevelUp") : nullptr;
    if (!need) return g_needs;
    for (auto& pair : table->GetRowMap()) {
        const int level = std::atoi(ue::utf8(pair.Key).c_str());
        if (level < 1 || level > 1000) continue;
        if (static_cast<int>(g_needs.size()) < level) g_needs.resize(static_cast<std::size_t>(level), 0);
        g_needs[static_cast<std::size_t>(level - 1)] = *reinterpret_cast<const std::int32_t*>(pair.Value + need->GetOffset_Internal());
    }
    return g_needs;
}

Snap snapshot(ue::UObject* pc) {
    Snap s;
    const auto* sc = core::schema::script();
    if (!sc || !pc) return s;
    auto* inv = sc->zpc_inventory.object(pc);
    auto* pp = sc->zpc_param.object(pc);
    auto* heal = sc->zpc_heal.object(pc);
    auto* pe = sc->zpc_passive.object(pc);
    if (!inv || !pp || !heal || !pe) return s;
    {
        reflect::Frame f(sc->pp_level);
        f.arg_bool(0, true);
        s.level = f.invoke(pp) ? f.ret<std::int32_t>() : 0;
    }
    const auto& needs = level_needs(*sc);
    s.exp_total = sc->pp_experience.read<std::int32_t>(pp);
    for (int l = 1; l < s.level && l <= static_cast<int>(needs.size()); ++l) s.exp_total += needs[static_cast<std::size_t>(l - 1)];
    for (const auto& p : reflect::map_pairs(inv, sc->inv_currencies)) {
        const int k = *p.key;
        if (k >= 0 && k < kCurrencies) s.currency[static_cast<std::size_t>(k)] = *reinterpret_cast<const std::int32_t*>(p.value);
    }
    for (std::size_t k = 0; k < 4; ++k) {
        auto* sub = sc->inv_sub[k].object(inv);
        if (!sub) return s;
        const auto& arr = reflect::array_at(sub, sc->binv_items);
        const auto stride = reflect::element_size(sc->binv_items);
        if (stride <= 0 || arr.num < 0 || arr.num > 4096) return s;
        for (int i = 0; i < arr.num; ++i) {
            const auto row = sc->item_row_name.read<FName>(arr.data + static_cast<std::ptrdiff_t>(i) * stride);
            const auto key = ue::name_key(row);
            ++s.counts[k][key];
            g_names.try_emplace(key, row);
        }
        if (k == 3)
            for (const auto& p : reflect::map_pairs(sub, sc->spirit_levels)) {
                const auto& id = *reinterpret_cast<const FName*>(p.key);
                const int lv = *reinterpret_cast<const std::int32_t*>(p.value);
                if (lv > 1) {
                    s.spirit_levels[ue::name_key(id)] = lv;
                    g_names.try_emplace(ue::name_key(id), id);
                }
            }
    }
    const auto& apt = reflect::array_at(pc, sc->zpc_unlocked);
    for (int i = 0; i < apt.num && i < 256; ++i) s.unlocked.set(apt.data[i]);
    s.heal = {sc->heal_max_count_level.read<std::int32_t>(heal), call_int(sc->heal_count_level, heal), call_int(sc->heal_power_level, heal)};
    s.slots = call_int(sc->pe_slot_count, pe);
    s.ok = true;
    return s;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))); }

std::uint64_t signature(const Snap& s) {
    std::uint64_t h = mix(0, static_cast<std::uint64_t>(s.level));
    for (const auto& m : s.counts)
        for (const auto& [k, n] : m) h = mix(mix(h, k), static_cast<std::uint64_t>(n));
    for (std::size_t i = 0; i < 256; ++i)
        if (s.unlocked.test(i)) h = mix(h, 1000 + i);
    for (const auto& [k, n] : s.spirit_levels) h = mix(mix(h, k), static_cast<std::uint64_t>(n));
    for (int v : s.heal) h = mix(h, static_cast<std::uint64_t>(v));
    return mix(h, static_cast<std::uint64_t>(s.slots));
}

// Seat matches P1: same level / unlocks / aptitude rows / spirit levels / prayer levels / slots and at
// least P1's rows of the additive inventories.
std::string aligned(const Snap& seat, const Snap& host) {
    if (seat.level != host.level) return std::format("level {} != {}", seat.level, host.level);
    if (seat.unlocked != host.unlocked) return "aptitude unlocks differ";
    if (seat.counts[0] != host.counts[0]) return "aptitude rows differ";
    if (seat.spirit_levels != host.spirit_levels) return "spirit levels differ";
    if (seat.heal != host.heal) return "prayer levels differ";
    if (seat.slots != host.slots) return std::format("relic slots {} != {}", seat.slots, host.slots);
    for (std::size_t k = 1; k < 4; ++k)
        for (const auto& [row, n] : host.counts[k]) {
            const auto it = seat.counts[k].find(row);
            if (it == seat.counts[k].end() || it->second < n) return std::format("{} rows missing", kKinds[k]);
        }
    return {};
}

struct Gains {
    std::int64_t exp = 0;
    std::array<std::int64_t, kCurrencies> currency{};
    std::array<std::map<std::uint32_t, int>, 4> items;
    std::map<std::uint32_t, int> spirit_levels;
    bool empty() const {
        if (exp > 0) return false;
        for (auto c : currency)
            if (c > 0) return false;
        for (const auto& m : items)
            if (!m.empty()) return false;
        return spirit_levels.empty();
    }
};

Gains gains(const Snap& before, const Snap& after) {
    Gains g;
    g.exp = std::max<std::int64_t>(0, after.exp_total - before.exp_total);
    for (int i = 0; i < kCurrencies; ++i) g.currency[static_cast<std::size_t>(i)] = std::max<std::int64_t>(0, after.currency[static_cast<std::size_t>(i)] - before.currency[static_cast<std::size_t>(i)]);
    for (std::size_t k = 0; k < 4; ++k)
        for (const auto& [row, n] : after.counts[k]) {
            const auto it = before.counts[k].find(row);
            const int d = n - (it == before.counts[k].end() ? 0 : it->second);
            if (d > 0) g.items[k][row] = d;
        }
    for (const auto& [id, lv] : after.spirit_levels) {
        const auto it = before.spirit_levels.find(id);
        if (lv > (it == before.spirit_levels.end() ? 1 : it->second)) g.spirit_levels[id] = lv;
    }
    return g;
}

// Writes P1 — only here, only vanilla acquisition entries, only increases (X-4, FR-PRG-2).
void grant(const core::schema::Script& s, const core::schema::Blueprint& b, ue::UObject* host, const Snap& host_snap, const Gains& g) {
    auto* inv = s.zpc_inventory.object(host);
    auto* pp = s.zpc_param.object(host);
    if (!inv || !pp) return;
    if (g.exp > 0) reflect::Frame(s.pp_add_experience).arg(0, static_cast<std::int32_t>(g.exp)).arg_bool(1, true).invoke(pp);
    for (int i = 0; i < kCurrencies; ++i)
        if (g.currency[static_cast<std::size_t>(i)] > 0)
            reflect::Frame(s.inv_add_currency).arg(0, static_cast<std::uint8_t>(i)).arg(1, static_cast<std::int32_t>(g.currency[static_cast<std::size_t>(i)])).invoke(inv);
    const std::array<ObjHandle, 4> tables{b.table_aptitude, b.table_parameter, b.table_passive, b.table_spirit};
    for (std::size_t k = 0; k < 4; ++k)
        for (const auto& [row, n] : g.items[k]) {
            const auto it = g_names.find(row);
            if (it == g_names.end()) continue;
            struct {
                ue::UObject* table;
                FName row;
            } handle{tables[k].get(), it->second};
            for (int i = 0; i < n; ++i) {
                reflect::Frame f(s.inv_add_item);
                std::memcpy(f.raw(0), &handle, sizeof(handle));
                f.invoke(inv);
            }
        }
    if (!g.spirit_levels.empty())
        if (auto* si = s.inv_sub[3].object(inv))
            for (const auto& [id, lv] : g.spirit_levels) {
                const auto have = host_snap.spirit_levels.find(id);
                if (have != host_snap.spirit_levels.end() && have->second >= lv) continue;
                reflect::Frame(s.spirit_set_level).arg_name(0, g_names[id]).arg(1, lv).invoke(si);
            }
    ++c_grants;
}

std::string mirror(ue::UObject* seat_pc, ue::UObject* host_pc, const Snap& seat, const Snap& host) {
    using core::profile::Profile;
    std::string err;
    auto hp = core::profile::read(host_pc, &err);
    if (!hp) return "cannot read P1: " + err;
    std::string failures;
    const auto run = [&](const Profile& p, bool exact) {
        const auto r = core::profile::apply(seat_pc, p, exact);
        if (!r.ok()) failures += r.text();
    };
    Profile exact_part, add_part;
    exact_part.aptitudes = hp->aptitudes;
    exact_part.spirit_levels = hp->spirit_levels;
    run(exact_part, true);
    add_part.parameters = hp->parameters;
    add_part.spirit_items = hp->spirit_items;
    if (hp->passives) add_part.passives = core::profile::Passives{std::nullopt, hp->passives->owned, std::nullopt};
    run(add_part, false);
    Profile when_different;
    if (seat.slots != host.slots && hp->passives) when_different.passives = core::profile::Passives{std::nullopt, std::nullopt, hp->passives->slots};
    if (hp->stats) {
        core::profile::Stats st;
        if (seat.level != host.level) st.level = hp->stats->level;
        if (seat.heal != host.heal) st.heal = hp->stats->heal; // only then: writing heal refills prayers
        if (st.level || st.heal) when_different.stats = st;
    }
    if (when_different.passives || when_different.stats) run(when_different, true);
    // Special attack revoked on P1's side: the vanilla Add only blocks growth, so clear stored MP.
    if (seat.unlocked.test(kSpecialAttack) && !host.unlocked.test(kSpecialAttack)) {
        const auto* s = core::schema::script();
        if (auto* p = s ? s->controller_pawn.object(seat_pc) : nullptr)
            if (auto* mp = s->zc_mp.object(p)) reflect::Frame(s->mp_reset).invoke(mp);
    }
    return failures;
}

void log_line(std::string l) {
    log::info("shared progress: {}", l);
    g_log.push_back(std::move(l));
    if (g_log.size() > 32) g_log.pop_front();
}

void run(const std::vector<std::int64_t>& slots) {
    const auto* s = core::schema::script();
    const auto* b = core::schema::blueprint();
    auto* host = core::controller(1);
    if (!s || !b || !host) return;
    std::map<std::int64_t, Snap> snaps;
    bool host_changed = false;
    Snap host_snap = snapshot(host);
    for (const auto slot : slots) {
        auto* pc = core::controller(slot);
        if (!pc) continue;
        auto snap = snapshot(pc);
        if (!snap.ok) continue;
        auto& st = g_seats[slot];
        if (st.has_baseline) {
            const auto g = gains(st.baseline, snap);
            if (!g.empty()) {
                grant(*s, *b, host, host_snap, g);
                host_changed = true;
                int rows = 0;
                for (const auto& m : g.items) rows += static_cast<int>(m.size());
                log_line(std::format("P{} -> P1: exp {} currency {}/{}/{} rows {} spirit levels {}", slot, g.exp, g.currency[0], g.currency[1], g.currency[2], rows, g.spirit_levels.size()));
                events::bus().push(events::Type::ProgressGrant, static_cast<std::int32_t>(slot), g.exp);
            }
        }
        snaps[slot] = std::move(snap);
    }
    if (host_changed) host_snap = snapshot(host);
    const auto host_sig = signature(host_snap);
    for (auto& [slot, snap] : snaps) {
        auto& st = g_seats[slot];
        const auto seat_sig = signature(snap);
        if (host_changed || st.synced_host != host_sig || st.synced_seat != seat_sig || !st.failure.empty()) {
            auto why = mirror(core::controller(slot), host, snap, host_snap);
            snap = snapshot(core::controller(slot));
            if (why.empty()) why = aligned(snap, host_snap);
            if (why.empty()) {
                st.synced_host = host_sig;
                st.synced_seat = signature(snap);
                if (!st.failure.empty()) log_line(std::format("P{} aligned again", slot));
                st.failure.clear();
                ++c_mirrored;
            } else {
                if (st.failure != why) log::warn("shared progress: P{} sync failed: {}", slot, why);
                st.failure = why;
                ++c_failures;
            }
        }
        st.baseline = snap;
        st.has_baseline = true;
    }
}

void phase(const frame::Tick&) {
    std::vector<std::int64_t> slots;
    for (const auto& seat : party::seats()) slots.push_back(seat.slot);
    if (!slots.empty()) run(slots);
}

} // namespace

std::optional<core::profile::Profile> shared_profile(std::string* err) {
    auto p = core::profile::read(core::controller(1), err);
    if (!p) return std::nullopt;
    p->spirits.reset();
    p->summon_set.reset();
    if (p->passives) p->passives->equipped.reset();
    return p;
}

void joined(std::int64_t slot) {
    auto& st = g_seats[slot];
    st = SeatState{};
    st.baseline = snapshot(core::controller(slot));
    st.has_baseline = st.baseline.ok;
}

void forget(std::int64_t slot) { g_seats.erase(slot); }
void forget_all() { g_seats.clear(); }

std::string sync(std::int64_t slot) {
    if (!core::controller(slot)) return "PX controller is not available";
    run({slot});
    const auto it = g_seats.find(slot);
    return it == g_seats.end() ? "no snapshot" : it->second.failure;
}

void tick() { phase({}); }

std::string describe() {
    std::string out = std::format("mirrored={} grants={} failures={}\n", c_mirrored.load(), c_grants.load(), c_failures.load());
    for (const auto& [slot, st] : g_seats)
        out += std::format("P{}: baseline={} synced={} {}\n", slot, st.has_baseline ? "yes" : "no", st.synced_host ? "yes" : "no", st.failure.empty() ? "ok" : "FAIL " + st.failure);
    for (const auto& l : g_log) out += "  " + l + "\n";
    return out;
}

void install() {
    frame::add_phase("party.progress", phase, [] { return party::count() > 0 && lifecycle::state() == lifecycle::State::Settled && !transit::active(); }, 1000);
}

} // namespace elmp::party::progress
