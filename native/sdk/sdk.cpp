// SDK (C20, ARCHITECTURE-V2 §12): per-mod ElmpApi_v1 tables. A function pointer cannot carry the caller,
// so each registered mod gets its own table built from template thunks bound to its index (kMaxMods).
// Mods are declared by `Mods/<name>/elmp.json` (scanned once at install): name, elmpApi, slots [lo, hi];
// the range is declared in the slot lease table (overlap with another mod → the mod is refused).

#include "sdk/sdk.h"
#include "sdk/elmp_sdk.h"

#include "party/seat.h"

#include "core/hitrules.h"
#include "core/permission.h"
#include "core/player.h"
#include "core/profile.h"
#include "core/spawn.h"
#include "core/vitals.h"

#include "coop/session.h"

#include "input/virtual_pad.h"

#include "engine/frame.h"
#include "engine/ue.h"

#include "base/events.h"
#include "base/json.h"
#include "base/log.h"
#include "base/settings.h"
#include "base/slots.h"

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace elmp::sdk {

namespace {

namespace fs = std::filesystem;
constexpr int kMaxMods = 16;

struct Sub {
    std::int32_t id = 0;
    ElmpEventFn fn = nullptr;
    void* user = nullptr;
};

struct Mod {
    std::string name, error;
    std::int64_t lo = 0, hi = 0;
    bool ok = false;
    fs::path storage;
    std::string last_error, buf;
    std::vector<Sub> subs;
    std::uint64_t cursor = 0;
    ElmpApi_v1 api{};
};

std::vector<Mod> g_mods;
std::int32_t g_next_sub = 1;

slots::Owner owner(const Mod& m) { return {m.name, slots::OwnerKind::ThirdParty}; }

ElmpResult fail(Mod& m, std::string why) {
    m.last_error = std::move(why);
    return 1;
}
ElmpResult done(Mod& m, const std::string& err) { return err.empty() ? (m.last_error.clear(), 0) : fail(m, err); }

bool mine(Mod& m, std::int64_t slot) {
    if (!ue::on_game_thread()) return fail(m, "not on the game thread"), false;
    if (slot < m.lo || slot > m.hi) return fail(m, std::format("slot {} outside the declared range [{}, {}]", slot, m.lo, m.hi)), false;
    return true;
}

template <int N>
struct Thunks {
    static Mod& m() { return g_mods[N]; }
    static ElmpResult acquire(std::int64_t slot) {
        if (!mine(m(), slot)) return 1;
        const auto r = core::leases().acquire(owner(m()), slot);
        return r == slots::Result::Ok ? done(m(), {}) : fail(m(), slots::describe(r));
    }
    static ElmpResult release(std::int64_t slot) {
        if (!mine(m(), slot)) return 1;
        return core::leases().release(owner(m()), slot) ? done(m(), {}) : fail(m(), "not held");
    }
    static bool held(std::int64_t slot) {
        if (!mine(m(), slot)) return false;
        if (core::leases().owner_of(slot) != m().name) return fail(m(), "acquire the slot first"), false;
        return true;
    }
    static ElmpResult spawn(std::int64_t slot, const char* prof, std::int32_t skin) {
        if (!held(slot)) return 1;
        core::profile::Profile p = core::profile::preset(core::profile::Preset::Default);
        const std::string text = prof ? prof : "";
        if (auto pr = core::profile::preset_named(text)) p = core::profile::preset(*pr);
        else if (!text.empty()) {
            json::Value v;
            std::string err;
            if (!json::parse(text, v, &err) || !core::profile::from_json(v, p, &err)) return fail(m(), "profile: " + err);
        }
        core::spawn::Options opt;
        if (skin > 0) opt.skin = skin;
        if (auto e = core::spawn::spawn(slot, opt); !e.empty()) return fail(m(), e);
        core::profile::reset(core::controller(slot), p);
        core::vitals::restore(slot);
        return done(m(), {});
    }
    static ElmpResult despawn(std::int64_t slot, std::int32_t keep) { return held(slot) ? done(m(), core::spawn::despawn(slot, keep != 0)) : 1; }
    static ElmpResult respawn(std::int64_t slot) { return held(slot) ? done(m(), core::spawn::respawn(slot, {})) : 1; }
    static ElmpResult profile_apply(std::int64_t slot, const char* text, std::int32_t exact) {
        if (!held(slot)) return 1;
        json::Value v;
        core::profile::Profile p;
        std::string err;
        if (!text || !json::parse(text, v, &err) || !core::profile::from_json(v, p, &err)) return fail(m(), "profile: " + err);
        auto* pc = core::controller(slot);
        if (!pc) return fail(m(), "no controller");
        core::profile::apply(pc, p, exact != 0);
        return done(m(), {});
    }
    static const char* profile_read(std::int64_t slot) {
        if (!mine(m(), slot)) return nullptr;
        std::string err;
        auto p = core::profile::read(core::controller(slot), &err);
        if (!p) return fail(m(), err), nullptr;
        m().buf = json::dump(core::profile::to_json(*p));
        return m().buf.c_str();
    }
    static ElmpResult set_input(std::int64_t slot, float x, float y, std::uint32_t held_bits) {
        if (!held(slot)) return 1;
        return done(m(), input::pad::set(slot, x, y, static_cast<std::int64_t>(held_bits)));
    }
    static const char* state(std::int64_t slot) {
        if (!mine(m(), slot)) return nullptr;
        const auto v = core::vitals::read(slot);
        const auto at = core::pawn(slot) ? core::location(core::pawn(slot)) : std::nullopt;
        m().buf = std::format(R"({{"slot":{},"alive":{},"hp":{},"maxHp":{},"mp":{},"pos":[{:.1f},{:.1f},{:.1f}],"online":"{}"}})", slot, v.alive ? "true" : "false",
                              v.hp, v.max_hp, v.mp, at ? at->x : 0.0f, at ? at->y : 0.0f, at ? at->z : 0.0f,
                              coop::role() == coop::Role::None ? "offline" : "unsupported");
        return m().buf.c_str();
    }
    static ElmpResult set_settings(const char* key, const char* value) {
        if (!key || !value) return fail(m(), "null argument");
        return done(m(), settings::set(key, value));
    }
    static ElmpResult set_permission(std::int64_t slot, const char* gate, std::int32_t allowed) {
        if (!held(slot)) return 1;
        core::permission::Gate g;
        if (!gate || !core::permission::gate_named(gate, g)) return fail(m(), "unknown gate");
        core::permission::set(slot, g, allowed != 0);
        return done(m(), {});
    }
    static ElmpResult set_team(std::int64_t slot, std::int32_t enemy) {
        if (!held(slot)) return 1;
        return done(m(), core::hitrules::bind(owner(m()), slot, enemy ? core::hitrules::Team::Enemy : core::hitrules::Team::Friendly));
    }
    static ElmpResult hit_rule(float damage, std::int32_t knockback) {
        auto e = settings::set("pvpDamage", std::format("{}", damage));
        if (e.empty()) e = settings::set("pvpKnockback", std::format("{}", knockback));
        return done(m(), e);
    }
    static std::int32_t subscribe(ElmpEventFn fn, void* user) {
        if (!fn) return fail(m(), "null callback"), 0;
        if (m().subs.empty()) m().cursor = events::bus().last_seq();
        m().subs.push_back({g_next_sub, fn, user});
        return g_next_sub++;
    }
    static ElmpResult unsubscribe(std::int32_t id) {
        return std::erase_if(m().subs, [id](const Sub& s) { return s.id == id; }) ? done(m(), {}) : fail(m(), "unknown subscription");
    }
    static const char* storage_path() {
        std::error_code ec;
        fs::create_directories(m().storage, ec);
        m().buf = m().storage.string();
        return m().buf.c_str();
    }
    static void log_fn(std::int32_t level, const char* text) {
        const auto t = std::format("[{}] {}", m().name, text ? text : "");
        level >= 2 ? log::error("{}", t) : level == 1 ? log::warn("{}", t) : log::info("{}", t);
    }
    static const char* last_error() { return m().last_error.c_str(); }

    static ElmpApi_v1 table() {
        return {ELMP_API_VERSION_1, sizeof(ElmpApi_v1), acquire, release, spawn, despawn, respawn, profile_apply, profile_read, set_input, state,
                set_settings, set_permission, set_team, hit_rule, subscribe, unsubscribe, storage_path, log_fn, last_error};
    }
};

template <int... N>
std::array<ElmpApi_v1, sizeof...(N)> tables(std::integer_sequence<int, N...>) {
    return {Thunks<N>::table()...};
}

void dispatch(const frame::Tick&) {
    std::array<events::Event, 64> evs;
    for (auto& m : g_mods) {
        if (m.subs.empty()) continue;
        const auto n = events::bus().since(m.cursor, evs);
        for (std::size_t i = 0; i < n; ++i) {
            const ElmpEvent ev{evs[i].seq, static_cast<std::uint16_t>(evs[i].type), evs[i].slot, evs[i].a, evs[i].b};
            for (const auto& s : m.subs) s.fn(&ev, s.user);
        }
    }
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

void install(const fs::path& mods_root, const fs::path& storage_root) {
    g_mods.clear();
    std::error_code ec;
    for (const auto& dir : fs::directory_iterator(mods_root, ec)) {
        const auto manifest = dir.path() / "elmp.json";
        if (!dir.is_directory() || dir.path().filename() == "ELMP" || !fs::exists(manifest, ec)) continue;
        if (static_cast<int>(g_mods.size()) >= kMaxMods) {
            log::warn("sdk: more than {} SDK mods; {} ignored", kMaxMods, dir.path().filename().string());
            continue;
        }
        Mod m;
        m.name = dir.path().filename().string();
        json::Value v;
        std::string err;
        if (!json::parse(read_file(manifest), v, &err)) m.error = "elmp.json: " + err;
        else {
            const auto* api = v.get("elmpApi");
            const auto* sl = v.get("slots");
            if (!api || api->number_or(0) != 1) m.error = "elmpApi must be 1";
            else if (!sl || !sl->is_array() || sl->arr.size() != 2) m.error = "slots must be [first, last]";
            else {
                m.lo = static_cast<std::int64_t>(sl->arr[0].number_or(0));
                m.hi = static_cast<std::int64_t>(sl->arr[1].number_or(0));
                m.error = core::leases().declare(owner(m), m.lo, m.hi); // overlap / reserved range → refused
            }
        }
        m.ok = m.error.empty();
        m.storage = storage_root / m.name;
        log::info("sdk: mod '{}' {}", m.name, m.ok ? std::format("registered (slots {}-{})", m.lo, m.hi) : "refused: " + m.error);
        g_mods.push_back(std::move(m));
    }
    const auto all = tables(std::make_integer_sequence<int, kMaxMods>{});
    for (std::size_t i = 0; i < g_mods.size(); ++i) g_mods[i].api = all[i];
    frame::add_phase("sdk.events", dispatch, [] {
        for (const auto& m : g_mods)
            if (!m.subs.empty()) return true;
        return false;
    });
}

const ElmpApi_v1* api_for(std::string_view mod_name, std::string* why) {
    for (auto& m : g_mods)
        if (m.name == mod_name) {
            if (!m.ok && why) *why = m.error;
            return m.ok ? &m.api : nullptr;
        }
    if (why) *why = "no Mods/" + std::string(mod_name) + "/elmp.json";
    return nullptr;
}

std::string describe() {
    std::string out = std::format("sdk: {} mod(s)\n", g_mods.size());
    for (const auto& m : g_mods)
        out += std::format("  {} {} slots {}-{} subs={}{}\n", m.name, m.ok ? "ok" : "refused", m.lo, m.hi, m.subs.size(), m.ok ? "" : " (" + m.error + ")");
    return out;
}

} // namespace elmp::sdk

extern "C" __declspec(dllexport) const void* ELMP_GetApi(std::uint32_t version, const char* mod_name) {
    if (version != ELMP_API_VERSION_1 || !mod_name) return nullptr;
    std::string why;
    const auto* api = elmp::sdk::api_for(mod_name, &why);
    if (!api) elmp::log::warn("sdk: ELMP_GetApi('{}') refused: {}", mod_name, why);
    return api;
}

// ---- Lua bindings (same per-mod tables; the calling mod is identified by its lua_State) ----
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <lua.hpp>
#include <unordered_map>

namespace elmp::sdk {

namespace {

using RC::LuaMadeSimple::Lua;
std::unordered_map<const void*, std::size_t> g_lua_mod; // lua_State* → index in g_mods

// callbacks (ExecuteInGameThread, hooks) run on Lua threads of the mod's states: key by the main thread
const void* main_thread(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    const void* m = lua_tothread(L, -1);
    lua_pop(L, 1);
    return m;
}

Mod* lua_mod(const Lua& lua) {
    auto it = g_lua_mod.find(main_thread(lua.get_lua_state()));
    return it == g_lua_mod.end() ? nullptr : &g_mods[it->second];
}
std::int64_t int_arg(const Lua& lua) { return lua.is_number() ? static_cast<std::int64_t>(lua.get_number()) : 0; }
double num_arg(const Lua& lua) { return lua.is_number() ? lua.get_number() : 0.0; }
std::string str_arg(const Lua& lua) { return lua.is_string() ? std::string(lua.get_string()) : std::string{}; }
int ret_result(const Lua& lua, ElmpResult r) { return lua.set_integer(r), 1; }
int ret_text(const Lua& lua, const char* s) {
    s ? lua.set_string(std::string_view(s)) : lua.set_nil();
    return 1;
}

#define ELMP_LUA(name, ...)                                                                                                            \
    int name(const Lua& lua) {                                                                                                          \
        Mod* m = lua_mod(lua);                                                                                                          \
        if (!m || !m->ok) return lua.set_string("ELMP SDK: mod not registered"), 1;                                                    \
        const ElmpApi_v1& api = m->api;                                                                                                 \
        __VA_ARGS__                                                                                                                     \
    }

ELMP_LUA(l_acquire, { return ret_result(lua, api.acquire(int_arg(lua))); })
ELMP_LUA(l_release, { return ret_result(lua, api.release(int_arg(lua))); })
ELMP_LUA(l_spawn, {
    const auto slot = int_arg(lua);
    const auto prof = str_arg(lua);
    const auto skin = int_arg(lua);
    return ret_result(lua, api.spawn(slot, prof.c_str(), static_cast<std::int32_t>(skin)));
})
ELMP_LUA(l_despawn, {
    const auto slot = int_arg(lua);
    return ret_result(lua, api.despawn(slot, static_cast<std::int32_t>(int_arg(lua))));
})
ELMP_LUA(l_respawn, { return ret_result(lua, api.respawn(int_arg(lua))); })
ELMP_LUA(l_profile_apply, {
    const auto slot = int_arg(lua);
    const auto text = str_arg(lua);
    return ret_result(lua, api.profileApply(slot, text.c_str(), static_cast<std::int32_t>(int_arg(lua))));
})
ELMP_LUA(l_profile_read, { return ret_text(lua, api.profileRead(int_arg(lua))); })
ELMP_LUA(l_set_input, {
    const auto slot = int_arg(lua);
    const auto x = static_cast<float>(num_arg(lua));
    const auto y = static_cast<float>(num_arg(lua));
    return ret_result(lua, api.setInput(slot, x, y, static_cast<std::uint32_t>(int_arg(lua))));
})
ELMP_LUA(l_state, { return ret_text(lua, api.state(int_arg(lua))); })
ELMP_LUA(l_set_settings, {
    const auto k = str_arg(lua);
    const auto v = str_arg(lua);
    return ret_result(lua, api.setSettings(k.c_str(), v.c_str()));
})
ELMP_LUA(l_set_permission, {
    const auto slot = int_arg(lua);
    const auto gate = str_arg(lua);
    return ret_result(lua, api.setPermission(slot, gate.c_str(), static_cast<std::int32_t>(int_arg(lua))));
})
ELMP_LUA(l_set_team, {
    const auto slot = int_arg(lua);
    return ret_result(lua, api.setTeam(slot, static_cast<std::int32_t>(int_arg(lua))));
})
ELMP_LUA(l_hit_rule, {
    const auto d = static_cast<float>(num_arg(lua));
    return ret_result(lua, api.hitRule(d, static_cast<std::int32_t>(int_arg(lua))));
})
ELMP_LUA(l_storage_path, { return ret_text(lua, api.storagePath()); })
ELMP_LUA(l_log, {
    const auto level = int_arg(lua);
    const auto text = str_arg(lua);
    api.log(static_cast<std::int32_t>(level), text.c_str());
    return 0;
})
ELMP_LUA(l_last_error, { return ret_text(lua, api.lastError()); })
// events for Lua: a JSON array of the events since the previous poll (per-mod cursor, like subscribe)
ELMP_LUA(l_poll_events, {
    if (!m->cursor) m->cursor = events::bus().last_seq();
    std::array<events::Event, 64> evs;
    const auto n = events::bus().since(m->cursor, evs);
    std::string out = "[";
    for (std::size_t i = 0; i < n; ++i)
        out += std::format("{}{{\"seq\":{},\"type\":\"{}\",\"slot\":{},\"a\":{},\"b\":{}}}", i ? "," : "", evs[i].seq, events::name(evs[i].type), evs[i].slot, evs[i].a, evs[i].b);
    return ret_text(lua, (out + "]").c_str());
})

#undef ELMP_LUA

} // namespace

void on_lua_start(std::wstring_view mod_name, Lua& lua, Lua& main, Lua& async, Lua* hook) {
    const auto name = log::utf8(std::wstring(mod_name));
    std::size_t idx = g_mods.size();
    for (std::size_t i = 0; i < g_mods.size(); ++i)
        if (g_mods[i].name == name && g_mods[i].ok) idx = i;
    if (idx == g_mods.size()) return; // no (valid) elmp.json: nothing injected
    static const std::pair<const char*, int (*)(const Lua&)> fns[] = {
        {"ELMP_Acquire", l_acquire},         {"ELMP_Release", l_release},       {"ELMP_Spawn", l_spawn},         {"ELMP_Despawn", l_despawn},
        {"ELMP_Respawn", l_respawn},         {"ELMP_ProfileApply", l_profile_apply}, {"ELMP_ProfileRead", l_profile_read}, {"ELMP_SetInput", l_set_input},
        {"ELMP_State", l_state},             {"ELMP_SetSettings", l_set_settings}, {"ELMP_SetPermission", l_set_permission}, {"ELMP_SetTeam", l_set_team},
        {"ELMP_HitRule", l_hit_rule},        {"ELMP_StoragePath", l_storage_path}, {"ELMP_Log", l_log},           {"ELMP_LastError", l_last_error},
        {"ELMP_PollEvents", l_poll_events},
    };
    for (Lua* state : {&lua, &main, &async, hook}) {
        if (!state) continue;
        g_lua_mod[main_thread(state->get_lua_state())] = idx;
        for (const auto& [n, f] : fns) state->register_function(n, f);
    }
    log::info("sdk: C bindings injected into Lua mod '{}' ({} functions)", name, std::size(fns));
}

} // namespace elmp::sdk
