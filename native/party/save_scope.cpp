#include "party/save_scope.h"

#include "engine/frame.h"
#include "engine/handle.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/clock.h"
#include "base/json.h"
#include "base/log.h"
#include "base/lua_literal.h"
#include "base/preflight.h"

#include <Unreal/UFunctionStructs.hpp>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <map>
#include <vector>

namespace elmp::party::save_scope {

using reflect::Kind;
namespace fs = std::filesystem;

namespace {

constexpr int kSchema = 1;
constexpr std::size_t kRecent = 64;
constexpr std::int64_t kSaveWindowUs = 30'000'000; // a .sav change this long after a vanilla save call counts as ours
constexpr std::uint8_t kIntentNewGame = 1;          // ELaunchGameIntent::NewGame

struct Entry {
    std::string id;
    std::vector<std::string> recent; // oldest first
};
std::map<std::string, Entry> g_index;
bool g_index_loaded = false;
std::string g_index_error;

struct Schema {
    reflect::PropRef slot_name, intent;
    reflect::FuncRef is_loaded, is_loading;
    ObjHandle subsystem_class;
};
std::optional<Schema> g_s;
std::string g_schema_error;
ObjHandle g_subsystem;

Status g_status;
std::string g_previous_id; // blocked: the saveId that Reuse would associate
std::string g_hash_now;    // blocked: fingerprint that Reuse / New would register
std::uint64_t g_bound_generation = ~0ull;
std::int64_t g_next_try = 0;
Bound g_on_bound = nullptr;

// Save registration
std::atomic<std::int64_t> g_last_save_call{0};
std::atomic<bool> g_prune{false};
bool g_hooked = false;
std::int64_t g_next_poll = 0;
fs::file_time_type g_mtime{};
std::string g_pending_hash;
int g_stable = 0;
std::uint64_t g_registered = 0, g_external = 0;

fs::path elmp_dir() {
    const auto saved = world::saved_dir();
    return saved.empty() ? fs::path{} : fs::path(saved) / L"ELMP" / L"ELMP";
}
fs::path sav_file(const std::string& name) {
    const auto saved = world::saved_dir();
    return saved.empty() ? fs::path{} : fs::path(saved) / L"SaveGames" / (log::wide(name) + L".sav");
}

std::string hash_of(const std::string& name) {
    const auto f = sav_file(name);
    std::error_code ec;
    if (f.empty() || !fs::exists(f, ec)) return {};
    try {
        return preflight::sha256_file(f);
    } catch (...) {
        return {};
    }
}

std::string new_id() {
    std::uint8_t b[16]{};
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string out;
    for (auto c : b) out += std::format("{:02x}", c);
    return out;
}

bool valid_id(const std::string& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

std::string save_index() {
    auto root = json::Value::object();
    root.set("schema", kSchema);
    auto slots = json::Value::object();
    for (const auto& [name, e] : g_index) {
        auto o = json::Value::object();
        o.set("id", e.id);
        auto r = json::Value::array();
        for (const auto& h : e.recent) r.push(h);
        o.set("recent", std::move(r));
        slots.set(name, std::move(o));
    }
    root.set("slots", std::move(slots));
    std::string err;
    if (!json_io::save(elmp_dir() / L"save_index.json", root, &err)) return "save_index.json: " + err;
    return {};
}

// Reads {slots: {name: {id, recent[], hashes{}?}}} from the JSON index or a v1 literal.
std::string read_slots(const json::Value& root, bool legacy) {
    const auto* slots = root.get("slots");
    if (!slots || !slots->is_object()) return "no slots table";
    for (const auto& [name, v] : slots->obj) {
        Entry e;
        if (const auto* id = v.get("id")) e.id = id->string_or("");
        if (!valid_id(e.id)) return std::format("slot {}: invalid saveId", name);
        if (const auto* r = v.get("recent"); r && r->is_array())
            for (const auto& h : r->arr) e.recent.emplace_back(h.string_or(""));
        if (legacy && e.recent.empty()) // v1 kept hashes{} too; order unknown there
            if (const auto* h = v.get("hashes"); h && h->is_object())
                for (const auto& [hash, _] : h->obj) e.recent.push_back(hash);
        std::erase_if(e.recent, [](const std::string& h) { return h.size() != 64; });
        g_index[name] = std::move(e);
    }
    return {};
}

std::string load_index() {
    if (g_index_loaded) return g_index_error;
    const auto dir = elmp_dir();
    if (dir.empty()) return "saved dir unknown";
    g_index_loaded = true;
    std::string err;
    if (auto root = json_io::load(dir / L"save_index.json", kSchema, &err)) {
        if (auto e = read_slots(*root, false); !e.empty()) g_index_error = "save_index.json: " + e;
        return g_index_error;
    }
    if (err != "missing") return g_index_error = "save_index.json: " + err;
    const auto legacy = dir / L"save_index.profile.lua";
    std::error_code ec;
    if (!fs::exists(legacy, ec)) return {};
    const auto text = json_io::read_file(legacy, &err);
    json::Value v;
    if (!text || !lua_literal::parse(*text, v, &err)) return g_index_error = "v1 save_index.profile.lua: " + err;
    if (const auto* s = v.get("schema"); !s || s->number_or(0) != 1) return g_index_error = "v1 save_index.profile.lua: unsupported schema";
    if (auto e = read_slots(v, true); !e.empty()) return g_index_error = "v1 save_index.profile.lua: " + e;
    log::info("save scope: migrated {} slot(s) from v1 save_index.profile.lua (the file is kept)", g_index.size());
    if (auto e = save_index(); !e.empty()) return g_index_error = e;
    return {};
}

void remember(Entry& e, const std::string& hash) {
    if (hash.empty() || std::find(e.recent.begin(), e.recent.end(), hash) != e.recent.end()) return;
    e.recent.push_back(hash);
    if (e.recent.size() > kRecent) e.recent.erase(e.recent.begin());
}

void prune() {
    std::size_t before = g_index.size();
    std::error_code ec;
    std::erase_if(g_index, [&](const auto& kv) { return !fs::exists(sav_file(kv.first), ec); });
    if (g_index.size() != before) {
        log::info("save scope: {} index entr(ies) dropped (vanilla save deleted / replaced); PX files kept", before - g_index.size());
        save_index();
    }
}

const Schema* schema() {
    if (g_s || !g_schema_error.empty()) return g_s ? &*g_s : nullptr;
    Schema s;
    auto* cls = reflect::find_class(L"/Script/Zenith.SaveSubsystem");
    s.subsystem_class = ObjHandle::of(cls);
    reflect::Resolver ss("SaveSubsystem", cls);
    s.slot_name = ss.prop(L"SaveSlotName", Kind::Str);
    s.is_loaded = ss.func(L"IsGameLoaded", 0);
    s.is_loading = ss.func(L"IsLoading", 0);
    reflect::Resolver gi("GameInstanceZenithBase", reflect::find_class(L"/Script/Zenith.GameInstanceZenithBase"));
    s.intent = gi.prop(L"LaunchGameIntent", Kind::Any, 1);
    for (const auto* r : {&ss, &gi})
        if (!r->ok() && g_schema_error.empty()) g_schema_error = r->failure();
    if (!g_schema_error.empty()) {
        log::error("save scope unavailable: {}", g_schema_error);
        return nullptr;
    }
    g_s = s;
    return &*g_s;
}

void hook_saves() {
    if (g_hooked) return;
    g_hooked = true;
    using RC::Unreal::UnrealScriptFunctionCallableContext;
    const auto mark = [](UnrealScriptFunctionCallableContext&, void*) { g_last_save_call = clock::now_us(); };
    const auto prune_later = [](UnrealScriptFunctionCallableContext&, void*) { g_prune = true; };
    for (const auto* path : {L"/Script/Zenith.SaveSubsystem:SaveGame", L"/Script/Zenith.SaveSubsystem:SaveGameAsync"})
        if (auto* f = static_cast<ue::UFunction*>(reflect::find_object(path))) f->RegisterPreHook(mark, nullptr);
        else log::warn("save scope: {} missing; saves through it are treated as external", log::utf8(path));
    for (const auto* path : {L"/Script/Zenith.SaveSubsystem:DeleteSaveDataAtSlotIndex", L"/Script/Zenith.SaveSubsystem:DeleteAllSaveData",
                             L"/Script/Zenith.SaveSubsystem:CopyGameDataToEmptySlot"})
        if (auto* f = static_cast<ue::UFunction*>(reflect::find_object(path))) f->RegisterPostHook(prune_later, nullptr);
}

std::string read_fstring(const reflect::PropRef& p, const void* base) {
    struct FStr {
        wchar_t* data;
        std::int32_t num, max;
    };
    const auto& s = *p.at<FStr>(base);
    return s.data && s.num > 1 ? log::utf8(std::wstring_view(s.data, static_cast<std::size_t>(s.num - 1))) : std::string();
}

void bound(const std::string& name, Entry& e, const std::string& hash, bool fresh) {
    remember(e, hash);
    if (auto err = save_index(); !err.empty()) log::warn("save scope: {}", err);
    g_status = {State::Bound, e.id, name, {}, false};
    g_mtime = {};
    g_stable = 0;
    log::info("save scope: {} bound to PX data {} ({})", name, e.id.substr(0, 8), fresh ? "new generation" : "known fingerprint");
    if (g_on_bound) g_on_bound(e.id, fresh);
}

void block(const std::string& name, std::string reason, bool can_reuse) {
    g_status = {State::Blocked, {}, name, std::move(reason), can_reuse};
    log::warn("save scope: {} blocked: {}", name, g_status.reason);
}

// One attempt; returns false while the save is not loaded yet.
bool try_bind() {
    const auto* s = schema();
    if (!s) {
        block("", "save subsystem unavailable: " + g_schema_error, false);
        return true;
    }
    auto* sub = g_subsystem.get();
    if (!sub) g_subsystem = ObjHandle::of(sub = world::find_instance(static_cast<ue::UClass*>(s->subsystem_class.get())));
    if (!sub) return false;
    reflect::Frame loaded(s->is_loaded), loading(s->is_loading);
    if (!loaded.invoke(sub) || !loaded.ret_bool() || !loading.invoke(sub) || loading.ret_bool()) return false;
    const auto name = read_fstring(s->slot_name, sub);
    if (name.empty() || name.size() > 64) return false;
    hook_saves();
    if (auto err = load_index(); !err.empty()) {
        block(name, "Old ELMP save index could not be read (" + err + "). Nothing was imported; the file was kept.", false);
        return true;
    }
    auto* gi = world::game_instance();
    const bool new_game = gi && s->intent.read<std::uint8_t>(gi) == kIntentNewGame;
    const auto hash = hash_of(name);
    auto it = g_index.find(name);
    if (new_game || it == g_index.end()) {
        auto& e = g_index[name];
        e = Entry{new_id(), {}};
        bound(name, e, hash, true);
        return true;
    }
    if (hash.empty()) {
        g_previous_id = it->second.id;
        block(name, "Cannot read the original save fingerprint. PX data was not changed; choose New PX data or Reuse previous PX data.", true);
        return true;
    }
    if (std::find(it->second.recent.begin(), it->second.recent.end(), hash) == it->second.recent.end()) {
        g_previous_id = it->second.id;
        g_hash_now = hash;
        block(name, "The original save does not match its last known state (replaced, restored or saved without ELMP). Choose New PX data or Reuse previous PX data.", true);
        return true;
    }
    bound(name, it->second, hash, false);
    return true;
}

void poll_saves(std::int64_t now) {
    if (g_status.state != State::Bound || now < g_next_poll) return;
    const bool saving = g_stable || now - g_last_save_call.load() <= kSaveWindowUs + 2'000'000;
    g_next_poll = now + (saving ? 1'000'000 : 10'000'000); // external changes only need counting
    const auto f = sav_file(g_status.slot_name);
    std::error_code ec;
    const auto t = fs::last_write_time(f, ec);
    if (ec) return;
    if (g_mtime == fs::file_time_type{}) {
        g_mtime = t;
        return;
    }
    if (t == g_mtime && g_stable == 0) return;
    if (t != g_mtime) { // changed: wait until it stays the same for one more sample (async saves write in steps)
        g_mtime = t;
        g_stable = 1;
        return;
    }
    g_stable = 0;
    const auto hash = hash_of(g_status.slot_name);
    if (hash.empty()) return;
    auto& e = g_index[g_status.slot_name];
    if (e.id != g_status.id) return; // the slot was re-associated meanwhile
    if (now - g_last_save_call.load() > kSaveWindowUs) {
        ++g_external;
        log::warn("save scope: {} changed without a vanilla save call; fingerprint not registered (next load will ask)", g_status.slot_name);
        return;
    }
    if (std::find(e.recent.begin(), e.recent.end(), hash) != e.recent.end()) return;
    remember(e, hash);
    ++g_registered;
    if (auto err = save_index(); !err.empty()) log::warn("save scope: {}", err);
}

void step(const frame::Tick& t) {
    if (g_prune.exchange(false) && g_index_loaded) prune();
    poll_saves(t.now_us);
    if (lifecycle::state() != lifecycle::State::Settled || g_bound_generation == lifecycle::generation() || t.now_us < g_next_try) return;
    g_next_try = t.now_us + 250'000;
    if (!world::p1_pawn()) return; // title screen: nothing loaded for play
    if (try_bind()) g_bound_generation = lifecycle::generation();
}

} // namespace

Status status() { return g_status; }

bool busy() {
    return lifecycle::state() == lifecycle::State::Settled && g_bound_generation != lifecycle::generation() && world::p1_pawn() != nullptr;
}

std::string choose(bool new_data) {
    if (g_status.state != State::Blocked) return "PX data is not waiting for a choice";
    const auto name = g_status.slot_name;
    if (name.empty()) return "no save slot";
    if (!new_data && !g_status.can_reuse) return "previous PX data cannot be reused here";
    auto& e = g_index[name];
    if (new_data) e = Entry{new_id(), {}};
    else if (e.id != g_previous_id) return "save index changed meanwhile";
    const auto hash = g_hash_now.empty() ? hash_of(name) : g_hash_now;
    g_hash_now.clear();
    log::info("save scope: {} — player chose {}", name, new_data ? "New PX data" : "Reuse previous PX data");
    bound(name, e, hash, new_data);
    return {};
}

std::string force_new() {
    if (g_status.slot_name.empty()) return "no save slot";
    auto& e = g_index[g_status.slot_name];
    e = Entry{new_id(), {}};
    bound(g_status.slot_name, e, hash_of(g_status.slot_name), true);
    return {};
}

void on_bound(Bound fn) { g_on_bound = fn; }

std::string describe() {
    const char* states[] = {"unbound", "bound", "blocked"};
    std::string out = std::format("save scope: {} slot={} id={} index={} entr(ies) registered={} external={}{}\n", states[static_cast<int>(g_status.state)],
                                  g_status.slot_name.empty() ? "-" : g_status.slot_name, g_status.id.empty() ? "-" : g_status.id, g_index.size(), g_registered,
                                  g_external, g_status.reason.empty() ? "" : "\n  reason: " + g_status.reason);
    if (const auto it = g_index.find(g_status.slot_name); it != g_index.end())
        out += std::format("  known fingerprints: {} (last {})\n", it->second.recent.size(), it->second.recent.empty() ? "-" : it->second.recent.back().substr(0, 12));
    return out;
}

void install() {
    frame::add_phase("party.save_scope", step);
    lifecycle::on_teardown([] {
        g_status = {};
        g_previous_id.clear();
        g_hash_now.clear();
        g_subsystem.reset();
    });
}

} // namespace elmp::party::save_scope
