#include "core/profile.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/handle.h"
#include "engine/reflect.h"

#include "base/log.h"

#include <Unreal/Engine/UDataTable.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <set>

namespace elmp::core::profile {

using namespace RC::Unreal;
using schema::Blueprint;
using schema::Script;

namespace {

// ---- data facts (EAptitudeType ↔ DT_Items_Aptitudes rows; v1 core profile/aptitudes) ----

struct AptitudeRow {
    std::uint8_t value; // EAptitudeType; 0 = none (Heal, summon_set_switch have no enum value)
    const char* row;
    const char* enum_name;
};
constexpr std::array<AptitudeRow, 15> kAptitudes{{
    {0, "Heal", nullptr},
    {10, "Jump", "Jump"},
    {11, "double_jump", "DoubleJump"},
    {200, "stumble", "Stumble"},
    {20, "Dodge", "Dodge"},
    {30, "dash", "Dash"},
    {31, "dash_attack", "DashAttack"},
    {40, "pound_attack", "PoundAttack"},
    {70, "swim", "Swim"},
    {75, "dive", "Dive"},
    {90, "wallgrab", "Wallgrab"},
    {100, "Hook", "Hook"},
    {110, "door_unlock", "DoorUnlock"},
    {0, "summon_set_switch", nullptr},
    {120, "special_attack", "SpecialAttack"},
}};
// New game (bInitialAptitude rows).
const std::vector<std::string> kInitial{"Heal", "Jump", "stumble", "swim", "summon_set_switch"};

bool ieq(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

const char* aptitude_row(std::string_view name) {
    for (const auto& a : kAptitudes)
        if (ieq(name, a.row) || (a.enum_name && ieq(name, a.enum_name)) || (a.value && name == std::to_string(a.value))) return a.row;
    return nullptr;
}

std::vector<std::string> all_aptitudes() {
    std::vector<std::string> v;
    for (const auto& a : kAptitudes) v.emplace_back(a.row);
    return v;
}

const std::vector<SpiritSlot> kDefaultSpirits{{1, 3, "s5000"}, {2, 3, "s5000"}}; // Black Knight on ATTACK, both sets

constexpr int kMinPassiveSlots = 2, kMaxPassiveSlots = 20;

// ---- engine helpers ----

struct RowHandle {
    ue::UObject* table;
    FName row;
};
static_assert(sizeof(RowHandle) == 16);

FName fname(std::string_view utf8) { return FName(log::wide(utf8).c_str(), FNAME_Add); }
std::string text(const FName& n) { return ue::utf8(n); }
bool is_none(const FName& n) { return ue::name_key(n) == 0; }

int call_int(const reflect::FuncRef& f, ue::UObject* self) {
    reflect::Frame fr(f);
    return fr.invoke(self) ? fr.ret<std::int32_t>() : 0;
}
ue::UObject* call_obj(const reflect::FuncRef& f, ue::UObject* self) {
    reflect::Frame fr(f);
    auto* o = fr.invoke(self) ? fr.ret<ue::UObject*>() : nullptr;
    return ue::alive(o) ? o : nullptr;
}

struct Ctx {
    const Script& s;
    const Blueprint& b;
    ue::UObject* pc;
    ue::UObject* inv;
};

std::vector<std::string> rows(const Script& s, ue::UObject* sub) {
    std::vector<std::string> out;
    if (!sub) return out;
    const int n = call_int(s.binv_count, sub);
    for (int i = 0; i < n; ++i) {
        reflect::Frame f(s.binv_handle_at);
        f.arg(0, i);
        if (!f.invoke(sub) || !f.ret_bool()) continue;
        RowHandle h;
        std::memcpy(&h, f.raw(1), sizeof(h));
        out.push_back(text(h.row));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool add_item(const Ctx& c, ue::UObject* table, std::string_view row) {
    reflect::Frame f(c.s.inv_add_item);
    RowHandle h{table, fname(row)};
    std::memcpy(f.raw(0), &h, sizeof(h));
    return f.invoke(c.inv) && f.ret_bool();
}

void remove_item(const Ctx& c, ue::UObject* sub, ue::UObject* table, std::string_view row) {
    reflect::Frame f(c.s.binv_remove);
    RowHandle h{table, fname(row)};
    std::memcpy(f.raw(0), &h, sizeof(h));
    f.invoke(sub);
}

bool aptitude_unlocked(const Ctx& c, std::uint8_t value) {
    reflect::Frame f(c.s.zpc_aptitude_unlocked);
    f.arg(0, value);
    return f.invoke(c.pc) && f.ret_bool();
}

std::map<std::string, int> counts(const std::vector<std::string>& v) {
    std::map<std::string, int> m;
    for (const auto& r : v) ++m[r];
    return m;
}

// Max level per spirit = row count of its SpiritLevelTable (DT_Items_Spirits rows can be walked safely;
// v1: walking DT_Items_Passives with ForEachRow crashed). Static data, cached against the table handle.
ObjHandle g_max_for;
std::map<std::string, int> g_max_levels;

const std::map<std::string, int>& max_levels(const Blueprint& b) {
    if (g_max_for == b.table_spirit && !g_max_levels.empty()) return g_max_levels;
    g_max_levels.clear();
    g_max_for = b.table_spirit;
    auto* dt = static_cast<UDataTable*>(b.table_spirit.get());
    if (!dt) return g_max_levels;
    UScriptStruct* row_struct = dt->GetRowStruct();
    auto* level_prop = row_struct ? reflect::find_property(row_struct, L"SpiritLevelTable") : nullptr;
    if (!level_prop) return g_max_levels;
    for (auto& pair : dt->GetRowMap()) {
        auto* table = *reinterpret_cast<UDataTable**>(pair.Value + level_prop->GetOffset_Internal());
        g_max_levels[text(pair.Key)] = table ? table->GetRowMap().Num() : 0;
    }
    return g_max_levels;
}

std::vector<SpiritSlot> equipped_spirits(const Script& s, ue::UObject* equip) {
    std::vector<SpiritSlot> out;
    if (!equip) return out;
    for (const auto& set : reflect::map_pairs(equip, s.se_sets)) {
        const int set_id = *set.key;
        for (const auto& slot : reflect::map_pairs(set.value, s.set_spirits)) {
            const auto id = s.spirit_id.read<FName>(slot.value);
            if (!is_none(id)) out.push_back({set_id, *slot.key, text(id)});
        }
    }
    std::sort(out.begin(), out.end(), [](const SpiritSlot& a, const SpiritSlot& b) { return a.set * 100 + a.command < b.set * 100 + b.command; });
    return out;
}

std::vector<std::string> equipped_passives(const Script& s, ue::UObject* pe) {
    std::vector<std::string> out;
    if (!pe) return out;
    const auto& arr = reflect::array_at(pe, s.pe_equipped);
    const auto stride = reflect::element_size(s.pe_equipped);
    if (stride <= 0 || arr.num < 0 || arr.num > 64) return out;
    for (int i = 0; i < arr.num; ++i) out.push_back(text(s.passive_id.read<FName>(arr.data + static_cast<std::ptrdiff_t>(i) * stride)));
    std::sort(out.begin(), out.end());
    return out;
}

// ---- fields ----

std::string apply_aptitudes(const Ctx& c, const std::vector<std::string>& list, bool exact) {
    std::set<std::string> want;
    std::string unknown;
    for (const auto& v : list) {
        if (const char* row = aptitude_row(v)) want.insert(row);
        else unknown += (unknown.empty() ? "" : ", ") + v;
    }
    if (!unknown.empty()) throw std::runtime_error("unknown aptitudes: " + unknown);
    auto* apt = call_obj(c.s.inv_aptitudes, c.inv);
    auto* table = c.b.table_aptitude.get();
    if (!apt || !table) throw std::runtime_error("aptitude inventory missing");
    std::set<std::string> have;
    for (auto& r : rows(c.s, apt)) have.insert(r);
    int added = 0, removed = 0;
    for (const auto& a : kAptitudes) {
        if (!a.value) continue;
        const bool unlocked = aptitude_unlocked(c, a.value);
        if (want.contains(a.row) && have.contains(a.row) && !unlocked) { // stale row without the unlock: re-add below
            remove_item(c, apt, table, a.row);
            have.erase(a.row);
            ++removed;
        } else if (exact && !want.contains(a.row) && !have.contains(a.row) && unlocked) { // unlock without a row: add so removal revokes it
            if (add_item(c, table, a.row)) {
                have.insert(a.row);
                ++added;
            }
        }
    }
    for (const auto& r : want)
        if (!have.contains(r) && add_item(c, table, r)) ++added;
    if (exact) {
        for (const auto& r : have)
            if (!want.contains(r)) {
                remove_item(c, apt, table, r);
                ++removed;
            }
    }
    return std::format("added {} removed {}", added, removed);
}

std::string align_items(const Ctx& c, ue::UObject* sub, ue::UObject* table, const std::vector<std::string>& list, bool exact, bool parameter) {
    if (!sub || !table) throw std::runtime_error("inventory missing");
    auto have = counts(rows(c.s, sub));
    const auto want = counts(list);
    int added = 0, removed = 0;
    if (exact) {
        for (auto& [row, n] : have) {
            const auto w = want.contains(row) ? want.at(row) : 0;
            if (n > w) { // RemoveItem drops every copy of the row
                remove_item(c, sub, table, row);
                removed += n;
                n = 0;
            }
        }
    }
    for (const auto& [row, n] : want) {
        for (int k = have.contains(row) ? have[row] : 0; k < n; ++k)
            if (add_item(c, table, row)) ++added;
    }
    // Removing a max-HP item does not lower MaxHP by itself; AddMaxHPBonusPercentage(0) runs the vanilla
    // recalculation (IDA 0x140C97540 → 0x140C98380).
    if (parameter && removed > 0) {
        if (auto* pp = c.s.zpc_param.object(c.pc)) reflect::Frame(c.s.pp_add_max_hp_bonus).arg(0, 0.0f).invoke(pp);
    }
    return std::format("added {} removed {}", added, removed);
}

std::string apply_spirit_levels(const Ctx& c, const Profile& p, bool exact) {
    auto* inv = call_obj(c.s.inv_spirits, c.inv);
    if (!inv) throw std::runtime_error("spirit inventory missing");
    const auto& max = max_levels(c.b);
    if (max.empty()) throw std::runtime_error("spirit level table unavailable");
    std::map<std::string, int> target;
    if (exact)
        for (const auto& [id, m] : max) target[id] = 1;
    if (p.spirit_levels_max) {
        for (const auto& [id, m] : max) target[id] = m;
    } else if (p.spirit_levels) {
        for (const auto& [id, level] : *p.spirit_levels) {
            auto it = max.find(id);
            if (it == max.end()) throw std::runtime_error("unknown spirit: " + id);
            const int lv = level <= 0 ? it->second : level; // 0 / negative = max
            if (lv < 1 || lv > it->second) throw std::runtime_error(std::format("spirit {} level must be 1..{}", id, it->second));
            target[id] = lv;
        }
    }
    int changed = 0;
    for (const auto& [id, level] : target) {
        reflect::Frame get(c.s.spirit_get_level);
        get.arg_name(0, fname(id)).arg_bool(1, true);
        const int current = get.invoke(inv) ? get.ret<std::int32_t>() : 0;
        if (current < level || (exact && current != level)) {
            reflect::Frame set(c.s.spirit_set_level);
            set.arg_name(0, fname(id)).arg(1, level);
            set.invoke(inv);
            ++changed;
        }
    }
    return std::format("changed {}", changed);
}

std::string apply_spirits(const Ctx& c, const Profile& p, bool exact) {
    auto* equip = c.s.zpc_spirit_equip.object(c.pc);
    if (!equip) throw std::runtime_error("SpiritEquipComponent missing");
    int equipped = 0, unequipped = 0;
    if (p.spirits) {
        std::set<std::pair<int, int>> seen;
        for (const auto& w : *p.spirits) {
            if (w.set < 1 || w.set > 2 || w.command < 1 || w.command > 9 || w.id.empty()) throw std::runtime_error("spirit entries need set 1..2, command 1..9 and an id");
            if (!seen.insert({w.set, w.command}).second) throw std::runtime_error(std::format("duplicate spirit slot {}:{}", w.set, w.command));
        }
        auto current = equipped_spirits(c.s, equip);
        const auto wanted_at = [&](int set, int command) -> const SpiritSlot* {
            for (const auto& w : *p.spirits)
                if (w.set == set && w.command == command) return &w;
            return nullptr;
        };
        const auto moved = [&](const SpiritSlot& cur) {
            for (const auto& w : *p.spirits)
                if (w.set == cur.set && w.id == cur.id && w.command != cur.command) return true;
            return false;
        };
        for (auto it = current.begin(); it != current.end();) {
            const auto* w = wanted_at(it->set, it->command);
            if ((exact && !w) || (w && w->id != it->id) || moved(*it)) {
                reflect::Frame f(c.s.se_unequip);
                f.arg(0, static_cast<std::uint8_t>(it->set)).arg(1, static_cast<std::uint8_t>(it->command));
                f.invoke(equip);
                ++unequipped;
                it = current.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& w : *p.spirits) {
            const bool present = std::any_of(current.begin(), current.end(), [&](const SpiritSlot& s) { return s.set == w.set && s.command == w.command; });
            if (present) continue;
            reflect::Frame f(c.s.se_equip);
            f.arg(0, static_cast<std::uint8_t>(w.set)).arg_name(1, fname(w.id)).arg(2, static_cast<std::uint8_t>(w.command));
            f.invoke(equip);
            ++equipped;
        }
    }
    if (p.summon_set) {
        reflect::Frame cur(c.s.se_current);
        const int now = cur.invoke(equip) ? cur.ret<std::uint8_t>() : 0;
        if (now != *p.summon_set) reflect::Frame(c.s.se_switch).arg(0, static_cast<std::uint8_t>(*p.summon_set)).invoke(equip);
    }
    return std::format("equipped {} unequipped {}", equipped, unequipped);
}

std::string apply_passives(const Ctx& c, const Passives& v, bool exact) {
    auto* pe = c.s.zpc_passive.object(c.pc);
    if (!pe) throw std::runtime_error("PassiveEquipComponent missing");
    if (v.slots && (*v.slots < kMinPassiveSlots || *v.slots > kMaxPassiveSlots))
        throw std::runtime_error(std::format("passive slots must be {}..{}", kMinPassiveSlots, kMaxPassiveSlots));
    if (exact && v.equipped) {
        const std::set<std::string> want(v.equipped->begin(), v.equipped->end());
        for (const auto& id : equipped_passives(c.s, pe))
            if (!want.contains(id)) reflect::Frame(c.s.pe_unequip).arg_name(0, fname(id)).invoke(pe);
    }
    if (v.slots) {
        const int now = call_int(c.s.pe_slot_count, pe);
        if (now < *v.slots || (exact && now != *v.slots)) reflect::Frame(c.s.pe_set_slot_count).arg(0, *v.slots).invoke(pe);
    }
    std::string owned_detail = "-";
    if (v.owned || v.equipped) {
        // owned ∪ equipped (equipped items always stay in the inventory)
        std::vector<std::string> list = v.owned ? *v.owned : std::vector<std::string>{};
        const auto equipped_now = v.equipped ? *v.equipped : equipped_passives(c.s, pe);
        for (const auto& id : equipped_now)
            if (std::find(list.begin(), list.end(), id) == list.end()) list.push_back(id);
        owned_detail = align_items(c, call_obj(c.s.inv_passives, c.inv), c.b.table_passive.get(), list, exact && v.owned.has_value(), false);
    }
    std::string failed;
    for (const auto& id : v.equipped ? *v.equipped : std::vector<std::string>{}) {
        reflect::Frame is(c.s.pe_is_equipped);
        is.arg_name(0, fname(id));
        if (is.invoke(pe) && is.ret_bool()) continue;
        reflect::Frame eq(c.s.pe_equip);
        eq.arg_name(0, fname(id));
        const int code = eq.invoke(pe) ? eq.ret<std::uint8_t>() : 3; // EPassiveEquipResult: 0 ok, 1 slots, 2 count limit, 3 invalid
        if (code) failed += std::format(" {}={}", id, code);
    }
    if (!failed.empty()) throw std::runtime_error("equip refused:" + failed);
    return std::format("owned [{}] slots {}", owned_detail, call_int(c.s.pe_slot_count, pe));
}

std::string apply_stats(const Ctx& c, const Stats& v, bool exact) {
    const auto positive = [](const std::optional<int>& x, const char* name) {
        if (x && *x < 1) throw std::runtime_error(std::string(name) + " must be a positive integer");
    };
    positive(v.level, "level");
    if (v.heal) {
        positive(v.heal->max_count_level, "heal.maxCountLevel");
        positive(v.heal->count_level, "heal.countLevel");
        positive(v.heal->power_level, "heal.powerLevel");
    }
    const auto wants = [&](int current, const std::optional<int>& target) { return target && (current < *target || (exact && current != *target)); };
    auto* pp = c.s.zpc_param.object(c.pc);
    if (!pp) throw std::runtime_error("ParameterPlayerComponent missing");
    const auto level = [&] {
        reflect::Frame f(c.s.pp_level);
        f.arg_bool(0, true);
        return f.invoke(pp) ? f.ret<std::int32_t>() : 0;
    };
    if (wants(level(), v.level)) reflect::Frame(c.s.pp_set_level).arg(0, *v.level).invoke(pp);
    // Experience has no setter: only aligned inside the profile's level, never past a level-up (the vanilla
    // level-up flow is not bypassed).
    if (v.experience && (!v.level || level() == *v.level)) {
        const int need = call_int(c.s.pp_need_exp, pp);
        if (*v.experience < 0 || (need > 0 && *v.experience >= need))
            throw std::runtime_error(std::format("experience {} is outside level {} (0..{})", *v.experience, level(), need - 1));
        if (wants(c.s.pp_experience.read<std::int32_t>(pp), v.experience)) *c.s.pp_experience.at<std::int32_t>(pp) = *v.experience;
    }
    if (v.heal) {
        auto* heal = c.s.zpc_heal.object(c.pc);
        if (!heal) throw std::runtime_error("HealComponent missing");
        // MaxHealCountLevel has no setter (v1 core-facts).
        if (wants(c.s.heal_max_count_level.read<std::int32_t>(heal), v.heal->max_count_level))
            *c.s.heal_max_count_level.at<std::int32_t>(heal) = *v.heal->max_count_level;
        if (wants(call_int(c.s.heal_count_level, heal), v.heal->count_level)) reflect::Frame(c.s.heal_set_count_level).arg(0, *v.heal->count_level).invoke(heal);
        if (wants(call_int(c.s.heal_power_level, heal), v.heal->power_level)) reflect::Frame(c.s.heal_set_power_level).arg(0, *v.heal->power_level).invoke(heal);
        reflect::Frame(c.s.heal_fully_restore).invoke(heal);
    }
    return std::format("level {} exp {}", level(), c.s.pp_experience.read<std::int32_t>(pp));
}

// ---- json helpers ----

json::Value strings(const std::vector<std::string>& v) {
    auto a = json::Value::array();
    for (const auto& s : v) a.push(s);
    return a;
}

bool read_strings(const json::Value& v, std::vector<std::string>& out, const char* what, std::string* err) {
    if (!v.is_array()) {
        if (err) *err = std::string(what) + " must be an array of strings";
        return false;
    }
    for (const auto& e : v.arr) {
        if (!e.is_string()) {
            if (err) *err = std::string(what) + " must be an array of strings";
            return false;
        }
        out.push_back(e.str);
    }
    return true;
}

std::optional<int> read_int(const json::Value* v) {
    if (!v || !v->is_number() || v->num != static_cast<double>(static_cast<int>(v->num))) return std::nullopt;
    return static_cast<int>(v->num);
}

} // namespace

// ---------------- presets / json ----------------

std::optional<Preset> preset_named(std::string_view name) {
    if (ieq(name, "baseline")) return Preset::Baseline;
    if (ieq(name, "default")) return Preset::Default;
    if (ieq(name, "full")) return Preset::Full;
    if (ieq(name, "max")) return Preset::Max;
    return std::nullopt;
}

Profile preset(Preset p) {
    Profile r;
    switch (p) {
    case Preset::Baseline:
        r.aptitudes = kInitial;
        r.spirits = std::vector<SpiritSlot>{};
        r.summon_set = 1;
        r.spirit_levels = std::map<std::string, int>{};
        r.passives = Passives{std::vector<std::string>{}, std::vector<std::string>{}, kMinPassiveSlots};
        r.parameters = std::vector<std::string>{};
        r.stats = Stats{1, 0, Heal{1, 1, 1}};
        break;
    case Preset::Default:
        r.aptitudes = kInitial;
        r.spirits = kDefaultSpirits;
        r.summon_set = 1;
        break;
    case Preset::Full:
    case Preset::Max:
        r.aptitudes = all_aptitudes();
        r.spirits = kDefaultSpirits;
        r.summon_set = 1;
        r.spirit_levels_max = p == Preset::Max;
        break;
    }
    return r;
}

Profile fresh(const Profile& p) {
    Profile r = preset(Preset::Baseline);
    if (p.aptitudes) r.aptitudes = p.aptitudes;
    if (p.parameters) r.parameters = p.parameters;
    if (p.spirit_items) r.spirit_items = p.spirit_items;
    if (p.spirit_levels || p.spirit_levels_max) {
        r.spirit_levels = p.spirit_levels;
        r.spirit_levels_max = p.spirit_levels_max;
    }
    if (p.spirits) r.spirits = p.spirits;
    if (p.summon_set) r.summon_set = p.summon_set;
    if (p.passives) {
        if (p.passives->equipped) r.passives->equipped = p.passives->equipped;
        if (p.passives->owned) r.passives->owned = p.passives->owned;
        if (p.passives->slots) r.passives->slots = p.passives->slots;
    }
    if (p.stats) {
        if (p.stats->level) r.stats->level = p.stats->level;
        if (p.stats->experience) r.stats->experience = p.stats->experience;
        if (p.stats->heal) {
            if (p.stats->heal->max_count_level) r.stats->heal->max_count_level = p.stats->heal->max_count_level;
            if (p.stats->heal->count_level) r.stats->heal->count_level = p.stats->heal->count_level;
            if (p.stats->heal->power_level) r.stats->heal->power_level = p.stats->heal->power_level;
        }
    }
    return r;
}

json::Value to_json(const Profile& p) {
    auto o = json::Value::object();
    if (p.aptitudes) o.set("aptitudes", strings(*p.aptitudes));
    if (p.parameters) o.set("parameters", strings(*p.parameters));
    if (p.spirit_items) o.set("spiritItems", strings(*p.spirit_items));
    if (p.spirit_levels_max) o.set("spiritLevels", "max");
    else if (p.spirit_levels) {
        auto m = json::Value::object();
        for (const auto& [id, lv] : *p.spirit_levels) m.set(id, lv);
        o.set("spiritLevels", std::move(m));
    }
    if (p.spirits) {
        auto a = json::Value::array();
        for (const auto& s : *p.spirits) {
            auto e = json::Value::object();
            e.set("set", s.set);
            e.set("command", s.command);
            e.set("id", s.id);
            a.push(std::move(e));
        }
        o.set("spirits", std::move(a));
    }
    if (p.summon_set) o.set("summonSet", *p.summon_set);
    if (p.passives) {
        auto e = json::Value::object();
        if (p.passives->equipped) e.set("equipped", strings(*p.passives->equipped));
        if (p.passives->owned) e.set("owned", strings(*p.passives->owned));
        if (p.passives->slots) e.set("slots", *p.passives->slots);
        o.set("passives", std::move(e));
    }
    if (p.stats) {
        auto e = json::Value::object();
        if (p.stats->level) e.set("level", *p.stats->level);
        if (p.stats->experience) e.set("experience", *p.stats->experience);
        if (p.stats->heal) {
            auto h = json::Value::object();
            if (p.stats->heal->max_count_level) h.set("maxCountLevel", *p.stats->heal->max_count_level);
            if (p.stats->heal->count_level) h.set("countLevel", *p.stats->heal->count_level);
            if (p.stats->heal->power_level) h.set("powerLevel", *p.stats->heal->power_level);
            e.set("heal", std::move(h));
        }
        o.set("stats", std::move(e));
    }
    return o;
}

bool from_json(const json::Value& v, Profile& out, std::string* err) {
    const auto fail = [&](std::string why) {
        if (err) *err = std::move(why);
        return false;
    };
    if (!v.is_object()) return fail("profile must be an object");
    Profile p;
    for (const auto& [key, val] : v.obj) {
        if (key == "aptitudes" || key == "parameters" || key == "spiritItems") {
            std::vector<std::string> list;
            if (!read_strings(val, list, key.c_str(), err)) return false;
            (key == "aptitudes" ? p.aptitudes : key == "parameters" ? p.parameters : p.spirit_items) = std::move(list);
        } else if (key == "spiritLevels") {
            if (val.is_string() && val.str == "max") p.spirit_levels_max = true;
            else if (val.is_object()) {
                std::map<std::string, int> m;
                for (const auto& [id, lv] : val.obj) {
                    if (lv.is_string() && lv.str == "max") m[id] = 0;
                    else if (auto n = read_int(&lv)) m[id] = *n;
                    else return fail("spiritLevels values must be integers or \"max\"");
                }
                p.spirit_levels = std::move(m);
            } else return fail("spiritLevels must be an object or \"max\"");
        } else if (key == "spirits") {
            if (!val.is_array()) return fail("spirits must be an array");
            std::vector<SpiritSlot> list;
            for (const auto& e : val.arr) {
                const auto set = read_int(e.get("set")), cmd = read_int(e.get("command"));
                const auto* id = e.get("id");
                if (!set || !cmd || !id || !id->is_string()) return fail("spirit entries need integer set / command and string id");
                list.push_back({*set, *cmd, id->str});
            }
            p.spirits = std::move(list);
        } else if (key == "summonSet") {
            p.summon_set = read_int(&val);
            if (!p.summon_set) return fail("summonSet must be an integer");
        } else if (key == "passives") {
            if (!val.is_object()) return fail("passives must be an object");
            Passives ps;
            for (const auto& [k, x] : val.obj) {
                if (k == "equipped" || k == "owned") {
                    std::vector<std::string> list;
                    if (!read_strings(x, list, k.c_str(), err)) return false;
                    (k == "equipped" ? ps.equipped : ps.owned) = std::move(list);
                } else if (k == "slots") {
                    ps.slots = read_int(&x);
                    if (!ps.slots) return fail("passives.slots must be an integer");
                } else return fail("unknown passives field: " + k);
            }
            p.passives = std::move(ps);
        } else if (key == "stats") {
            if (!val.is_object()) return fail("stats must be an object");
            Stats st;
            for (const auto& [k, x] : val.obj) {
                if (k == "level") st.level = read_int(&x);
                else if (k == "experience") st.experience = read_int(&x);
                else if (k == "heal") {
                    if (!x.is_object()) return fail("stats.heal must be an object");
                    Heal h;
                    for (const auto& [hk, hx] : x.obj) {
                        if (hk == "maxCountLevel") h.max_count_level = read_int(&hx);
                        else if (hk == "countLevel") h.count_level = read_int(&hx);
                        else if (hk == "powerLevel") h.power_level = read_int(&hx);
                        else return fail("unknown stats.heal field: " + hk);
                    }
                    st.heal = h;
                } else return fail("unknown stats field: " + k);
            }
            p.stats = st;
        } else return fail("unknown profile field: " + key);
    }
    out = std::move(p);
    return true;
}

// ---------------- read / apply ----------------

bool Report::ok() const {
    if (!refused.empty()) return false;
    for (const auto& f : fields)
        if (!f.ok) return false;
    return true;
}

std::string Report::text() const {
    if (!refused.empty()) return "refused: " + refused;
    std::string out;
    for (const auto& f : fields) out += std::format("{} {}: {}\n", f.ok ? "ok  " : "FAIL", f.field, f.detail);
    return out.empty() ? "nothing to apply\n" : out;
}

std::optional<Profile> read(ue::UObject* pc, std::string* err) {
    const auto* s = schema::script();
    const auto* b = schema::blueprint();
    if (!s || !b || !pc) {
        if (err) *err = !pc ? "no controller" : "core schema unresolved: " + schema::status();
        return std::nullopt;
    }
    auto* inv = s->zpc_inventory.object(pc);
    if (!inv) {
        if (err) *err = "InventoryComponent missing";
        return std::nullopt;
    }
    Profile p;
    p.aptitudes = rows(*s, call_obj(s->inv_aptitudes, inv));
    p.parameters = rows(*s, call_obj(s->inv_parameters, inv));
    std::map<std::string, int> levels;
    auto* si = call_obj(s->inv_spirits, inv);
    p.spirit_items = rows(*s, si);
    if (si)
        for (const auto& pr : reflect::map_pairs(si, s->spirit_levels)) levels[text(*reinterpret_cast<const FName*>(pr.key))] = *reinterpret_cast<const std::int32_t*>(pr.value);
    p.spirit_levels = std::move(levels);
    auto* equip = s->zpc_spirit_equip.object(pc);
    p.spirits = equipped_spirits(*s, equip);
    if (equip) {
        reflect::Frame cur(s->se_current);
        if (cur.invoke(equip)) p.summon_set = cur.ret<std::uint8_t>();
    }
    if (auto* pe = s->zpc_passive.object(pc)) p.passives = Passives{equipped_passives(*s, pe), rows(*s, call_obj(s->inv_passives, inv)), call_int(s->pe_slot_count, pe)};
    if (auto* pp = s->zpc_param.object(pc)) {
        Stats st;
        reflect::Frame lv(s->pp_level);
        lv.arg_bool(0, true);
        if (lv.invoke(pp)) st.level = lv.ret<std::int32_t>();
        st.experience = s->pp_experience.read<std::int32_t>(pp);
        if (auto* heal = s->zpc_heal.object(pc))
            st.heal = Heal{s->heal_max_count_level.read<std::int32_t>(heal), call_int(s->heal_count_level, heal), call_int(s->heal_power_level, heal)};
        p.stats = st;
    }
    return p;
}

Report apply(ue::UObject* pc, const Profile& p, bool exact) {
    Report r;
    const auto* s = schema::script();
    const auto* b = schema::blueprint();
    if (!s || !b) {
        r.refused = "core schema unresolved: " + schema::status();
        return r;
    }
    if (!is_elmp_controller(pc)) {
        r.refused = "refusing to modify a controller not created by ELMP (P1 is read-only)";
        return r;
    }
    auto* inv = s->zpc_inventory.object(pc);
    if (!inv) {
        r.refused = "InventoryComponent missing";
        return r;
    }
    const Ctx c{*s, *b, pc, inv};
    const auto field = [&](const char* name, bool present, auto&& fn) {
        if (!present) return;
        try {
            r.fields.push_back({name, true, fn()});
        } catch (const std::exception& e) {
            r.fields.push_back({name, false, e.what()});
        }
    };
    field("aptitudes", p.aptitudes.has_value(), [&] { return apply_aptitudes(c, *p.aptitudes, exact); });
    field("parameters", p.parameters.has_value(), [&] { return align_items(c, call_obj(s->inv_parameters, inv), b->table_parameter.get(), *p.parameters, exact, true); });
    field("spiritItems", p.spirit_items.has_value(), [&] { return align_items(c, call_obj(s->inv_spirits, inv), b->table_spirit.get(), *p.spirit_items, exact, false); });
    field("spiritLevels", p.spirit_levels.has_value() || p.spirit_levels_max, [&] { return apply_spirit_levels(c, p, exact); });
    field("spirits", p.spirits.has_value() || p.summon_set.has_value(), [&] { return apply_spirits(c, p, exact); });
    field("passives", p.passives.has_value(), [&] { return apply_passives(c, *p.passives, exact); });
    field("stats", p.stats.has_value(), [&] { return apply_stats(c, *p.stats, exact); });
    return r;
}

Report reset(ue::UObject* pc, const Profile& p) { return apply(pc, fresh(p), true); }

} // namespace elmp::core::profile
