#include "party/storage.h"

#include "party/save_scope.h"
#include "party/seat.h"

#include "core/player.h"
#include "core/profile.h"
#include "core/skin.h"

#include "engine/lifecycle.h"
#include "engine/world.h"

#include "base/json.h"
#include "base/log.h"
#include "base/lua_literal.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <set>

namespace elmp::party::storage {

namespace fs = std::filesystem;

namespace {

constexpr int kSchema = 1;

struct Character {
    bool active = true;
    Source source = Source::Devices;
    Team team = Team::Friendly;
    int skin = 1;
    std::vector<core::profile::SpiritSlot> spirits;
    int summon_set = 1;
    std::vector<std::string> passives;
    bool unavailable = false;
};

std::string g_id;
std::map<std::int64_t, Character> g_chars;
std::string g_error; // PX data of the bound save unreadable → blocked
bool g_restoring = false;
std::string g_note;

fs::path file_of(const std::string& id, const wchar_t* ext) {
    const auto saved = world::saved_dir();
    return saved.empty() ? fs::path{} : fs::path(saved) / L"ELMP" / L"ELMP" / (L"party_" + log::wide(id) + ext);
}

// One character record (JSON or v1 literal shape); false = unusable (dropped with a log line).
bool parse_character(const json::Value& c, bool legacy, Character& out) {
    if (!c.is_object()) return false;
    const auto* active = c.get("active");
    const auto* input = c.get("input");
    const auto* team = c.get(legacy ? "identity" : "team");
    const auto* skin = c.get("skin");
    const auto* eq = c.get("equipment");
    if (!active || !active->is_bool() || !input || !team || !skin || !skin->is_number() || !eq || !eq->is_object()) return false;
    out.active = active->b;
    const auto in = input->string_or("");
    if (in != "devices" && in != "script") return false;
    out.source = in == "script" ? Source::Script : Source::Devices;
    const auto t = team->string_or("");
    if (t != "friendly" && t != "enemy") return false;
    out.team = t == "enemy" ? Team::Enemy : Team::Friendly;
    out.skin = static_cast<int>(skin->num);
    if (!core::skin::valid(out.skin)) return false;
    const auto* set = eq->get("summonSet");
    out.summon_set = set ? static_cast<int>(set->number_or(0)) : 0;
    if (out.summon_set != 1 && out.summon_set != 2) return false;
    std::set<std::pair<int, int>> seen;
    if (const auto* sp = eq->get("spirits"); sp && sp->is_array()) {
        for (const auto& s : sp->arr) {
            const int st = static_cast<int>(s.get("set") ? s.get("set")->number_or(0) : 0);
            const int cmd = static_cast<int>(s.get("command") ? s.get("command")->number_or(0) : 0);
            const auto id = std::string(s.get("id") ? s.get("id")->string_or("") : "");
            if ((st != 1 && st != 2) || cmd < 3 || cmd > 5 || id.empty() || !seen.insert({st, cmd}).second) return false;
            out.spirits.push_back({st, cmd, id});
        }
    } else if (!(sp && sp->is_object() && sp->obj.empty())) return false; // v1 wrote {} for an empty list
    if (const auto* ps = eq->get("passives"); ps && ps->is_array()) {
        for (const auto& p : ps->arr)
            if (p.is_string()) out.passives.push_back(p.str);
            else return false;
    } else if (!(ps && ps->is_object() && ps->obj.empty())) return false;
    out.unavailable = c.get("unavailable") && c.get("unavailable")->bool_or(false);
    return true;
}

std::string load_doc(const json::Value& root, bool legacy) {
    if (const auto* s = root.get("schema"); !s || s->number_or(0) != kSchema) return "unsupported schema";
    if (const auto* id = root.get("saveId"); !id || id->string_or("") != g_id) return "saveId does not match the save index";
    const auto* chars = root.get("characters");
    if (!chars || !chars->is_object()) return "no characters table";
    for (const auto& [key, c] : chars->obj) {
        std::int64_t slot = 0;
        try {
            slot = std::stoll(key);
        } catch (...) {
        }
        Character ch;
        if (slot < 2 || slot > 100 || std::to_string(slot) != key || !parse_character(c, legacy, ch)) {
            log::warn("storage: dropping invalid PX record {}", key);
            continue;
        }
        g_chars[slot] = std::move(ch);
    }
    return {};
}

std::string flush() {
    if (g_id.empty() || !g_error.empty()) return "no PX data bound";
    auto root = json::Value::object();
    root.set("schema", kSchema);
    root.set("saveId", g_id);
    auto chars = json::Value::object();
    for (const auto& [slot, c] : g_chars) {
        auto o = json::Value::object();
        o.set("active", c.active);
        o.set("input", c.source == Source::Script ? "script" : "devices");
        o.set("team", c.team == Team::Enemy ? "enemy" : "friendly");
        o.set("skin", c.skin);
        auto eq = json::Value::object();
        auto sp = json::Value::array();
        for (const auto& s : c.spirits) {
            auto e = json::Value::object();
            e.set("set", s.set);
            e.set("command", s.command);
            e.set("id", s.id);
            sp.push(std::move(e));
        }
        eq.set("spirits", std::move(sp));
        eq.set("summonSet", c.summon_set);
        auto ps = json::Value::array();
        for (const auto& p : c.passives) ps.push(p);
        eq.set("passives", std::move(ps));
        o.set("equipment", std::move(eq));
        if (c.unavailable) o.set("unavailable", true);
        chars.set(std::to_string(slot), std::move(o));
    }
    root.set("characters", std::move(chars));
    std::string err;
    if (!json_io::save(file_of(g_id, L".json"), root, &err)) {
        log::error("storage: {}", err);
        return err;
    }
    return {};
}

// Equipment filtered by what P1 owns; returns the omitted ids.
std::vector<std::string> apply_equipment(std::int64_t slot, const Character& c) {
    std::vector<std::string> omitted;
    std::string err;
    const auto p1 = core::profile::read(core::controller(1), &err);
    if (!p1) return {"(P1 profile unreadable: " + err + ")"};
    const auto owned = p1->spirit_items.value_or(std::vector<std::string>{});
    const auto relics = p1->passives && p1->passives->owned ? *p1->passives->owned : std::vector<std::string>{};
    core::profile::Profile p;
    p.spirits = std::vector<core::profile::SpiritSlot>{};
    for (const auto& s : c.spirits)
        if (std::find(owned.begin(), owned.end(), s.id) != owned.end()) p.spirits->push_back(s);
        else omitted.push_back(s.id);
    p.summon_set = c.summon_set;
    std::vector<std::string> eq;
    for (const auto& id : c.passives)
        if (std::find(relics.begin(), relics.end(), id) != relics.end()) eq.push_back(id);
        else omitted.push_back(id);
    p.passives = core::profile::Passives{eq, std::nullopt, std::nullopt};
    if (const auto r = core::profile::apply(core::controller(slot), p, true); !r.ok()) omitted.push_back("(apply: " + r.text() + ")");
    if (const auto now = core::profile::read(core::controller(slot), &err); now && now->passives && now->passives->equipped && now->passives->equipped->size() != eq.size())
        omitted.push_back("(relic slots)");
    return omitted;
}

void restore() {
    g_restoring = true;
    int joined = 0;
    for (auto& [slot, c] : g_chars) {
        if (!c.active) continue;
        JoinOptions o;
        o.slot = slot;
        o.source = c.source;
        o.team = c.team;
        if (auto e = party::join(o, nullptr); !e.empty()) {
            log::warn("storage: P{} could not re-join: {}", slot, e);
            continue;
        }
        const auto omitted = apply_equipment(slot, c);
        c.unavailable = !omitted.empty();
        if (!omitted.empty()) {
            std::string list;
            for (const auto& o2 : omitted) list += " " + o2;
            g_note = std::format("P{} unavailable equipment:{}", slot, list);
            log::warn("storage: {}", g_note);
        }
        if (auto e = core::skin::apply(core::pawn(slot), c.skin); !e.empty()) log::warn("storage: P{} skin: {}", slot, e);
        ++joined;
    }
    g_restoring = false;
    if (joined) log::info("storage: {} PX re-joined from party_{}", joined, g_id.substr(0, 8));
}

void on_bound(const std::string& id, bool fresh) {
    g_id = id;
    g_chars.clear();
    g_error.clear();
    g_note.clear();
    std::string err;
    const auto json_file = file_of(id, L".json");
    if (auto root = json_io::load(json_file, kSchema, &err)) {
        if (auto e = load_doc(*root, false); !e.empty()) g_error = std::format("PX data for this save could not be read ({}). Nothing was loaded; the file was kept.", e);
    } else if (err != "missing") {
        g_error = std::format("PX data for this save could not be read ({}). Nothing was loaded; the file was kept.", err);
    } else if (!fresh) {
        const auto legacy = file_of(id, L".profile.lua");
        std::error_code ec;
        if (fs::exists(legacy, ec)) {
            json::Value v;
            const auto text = json_io::read_file(legacy, &err);
            std::string why = !text ? err : (!lua_literal::parse(*text, v, &err) ? err : load_doc(v, true));
            if (!why.empty()) {
                g_chars.clear();
                g_error = std::format("Old ELMP profile for this save could not be read ({}). Nothing was imported; the file was kept.", why);
            } else {
                log::info("storage: migrated {} PX record(s) from v1 party_{}.profile.lua (the file is kept)", g_chars.size(), id.substr(0, 8));
                flush();
            }
        }
    }
    if (!g_error.empty()) {
        log::warn("storage: {}", g_error);
        return;
    }
    restore();
}

} // namespace

void capture(std::int64_t slot, bool active, bool edited) {
    if (g_restoring || g_id.empty() || !g_error.empty() || slot < 2 || slot > 100) return;
    const auto* seat = party::seat(slot);
    auto& c = g_chars[slot];
    c.active = active;
    if (seat) c.source = seat->source, c.team = seat->team;
    if (const int sk = core::skin::current(core::pawn(slot)); core::skin::valid(sk)) c.skin = sk;
    std::string err;
    if (const auto p = core::profile::read(core::controller(slot), &err)) {
        if (p->summon_set) c.summon_set = *p->summon_set;
        if (!c.unavailable || edited) { // keep the stored list while P1 lacks some of it, unless the player re-equipped
            c.spirits = p->spirits.value_or(std::vector<core::profile::SpiritSlot>{});
            std::erase_if(c.spirits, [](const auto& s) { return s.set < 1 || s.set > 2 || s.command < 3 || s.command > 5; });
            c.passives = p->passives && p->passives->equipped ? *p->passives->equipped : std::vector<std::string>{};
            c.unavailable = false;
        }
    }
    flush();
}

std::string join_allowed() {
    const auto s = save_scope::status();
    if (s.state == save_scope::State::Unbound) return "the loaded save is not identified yet";
    if (s.state == save_scope::State::Blocked) return "PX data for this save is waiting for a choice (elmp storage / ELMP manager)";
    if (!g_error.empty()) return "PX data for this save could not be read (elmp storage)";
    return {};
}

View view() {
    const auto s = save_scope::status();
    View v;
    v.id = s.id.empty() ? std::string() : s.id.substr(0, 8);
    v.blocked = s.state == save_scope::State::Blocked || !g_error.empty();
    v.can_reuse = s.state == save_scope::State::Blocked && s.can_reuse;
    v.message = s.state == save_scope::State::Blocked ? s.reason : !g_error.empty() ? g_error : g_note;
    return v;
}

std::string choose(bool new_data) {
    if (!g_error.empty()) {
        if (!new_data) return "the stored PX data is unreadable; only New PX data is possible";
        g_error.clear();
        return save_scope::force_new(); // new generation; the unreadable file stays where it is
    }
    return save_scope::choose(new_data);
}

std::string describe() {
    std::string out = save_scope::describe();
    out += std::format("storage: id={} characters={}{}{}\n", g_id.empty() ? "-" : g_id.substr(0, 8), g_chars.size(), g_error.empty() ? "" : "\n  error: " + g_error,
                       g_note.empty() ? "" : "\n  note: " + g_note);
    for (const auto& [slot, c] : g_chars) {
        std::string sp;
        for (const auto& s : c.spirits) sp += std::format(" {}:{}={}", s.set, s.command, s.id);
        out += std::format("  P{}: active={} {} {} skin={} set={} spirits:{} relics={}{}\n", slot, c.active, c.source == Source::Script ? "script" : "devices",
                           c.team == Team::Enemy ? "enemy" : "friendly", c.skin, c.summon_set, sp.empty() ? " -" : sp, c.passives.size(),
                           c.unavailable ? " (unavailable items kept)" : "");
    }
    return out;
}

void install() {
    save_scope::on_bound(&on_bound);
    lifecycle::on_teardown([] {
        g_id.clear();
        g_chars.clear();
        g_error.clear();
        g_note.clear();
    });
}

} // namespace elmp::party::storage
