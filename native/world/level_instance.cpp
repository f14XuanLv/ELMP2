#include "world/level_instance.h"

#include "core/player.h"

#include "engine/handle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/log.h"

#include <cctype>
#include <cstring>
#include <format>
#include <map>

namespace elmp::world::level_instance {

namespace {

constexpr std::string_view kPrefix = "ELMP_BG_";
constexpr const char* kParts[] = {"GEO", "DESIGN", "GAMEPLAY"};

struct Loaded {
    std::vector<ObjHandle> levels;
    float offset_y = 0;
};
std::map<std::string, Loaded, std::less<>> g_maps;

std::string area_name(std::string key) {
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key.empty()) return key;
    key[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
    return key;
}

} // namespace

std::vector<Part> parts(std::string_view map_id) {
    // map_<area>_<rest> → /Game/_Zenith/Maps/AreaMaps/<Area>/<Area>_<rest>/<Area>_<rest>_<Part> (Host2 church_08:
    // PackageNameToLoad of the vanilla instances; the map also has _AUDIO / _EVENT / _Map / <Area>_Common_LIGHT,
    // not loaded in the background: no sound / light needed, events stay fail closed)
    if (map_id.size() < 6 || (map_id.substr(0, 4) != "map_" && map_id.substr(0, 4) != "Map_")) return {};
    const auto rest0 = map_id.substr(4);
    const auto us = rest0.find('_');
    if (us == std::string_view::npos || us == 0 || us + 1 >= rest0.size()) return {};
    const auto area = area_name(std::string(rest0.substr(0, us)));
    const auto folder = std::format("{}_{}", area, rest0.substr(us + 1));
    std::vector<Part> out;
    for (const char* p : kParts)
        out.push_back({p, std::format("/Game/_Zenith/Maps/AreaMaps/{}/{}/{}_{}", area, folder, folder, p), std::format("{}{}__{}", kPrefix, map_id, p)});
    return out;
}

std::string load(std::string_view map_id, float offset_y) {
    if (loaded(map_id)) return {};
    const auto ps = parts(map_id);
    if (ps.empty()) return "unsupported map id";
    static reflect::FuncRef fn = reflect::Resolver("LevelStreamingDynamic", reflect::find_class(L"/Script/Engine.LevelStreamingDynamic")).func(L"LoadLevelInstance", 6); // nparms excludes the return value
    auto* cdo = reflect::find_object(L"/Script/Engine.Default__LevelStreamingDynamic");
    if (!fn.ok() || !cdo) return "LoadLevelInstance unavailable";
    Loaded rec;
    rec.offset_y = offset_y;
    for (const auto& p : ps) {
        reflect::Frame f(fn);
        f.arg(0, world::world()).arg_string(1, log::wide(p.package)).arg(2, core::Vec3{0.0f, offset_y, 0.0f});
        // Rotation (arg 3) stays zero; bOutSuccess (arg 4) is an out bool
        f.arg_string(5, log::wide(p.instance));
        if (!f.invoke(cdo)) return "LoadLevelInstance call failed (" + p.part + ")";
        auto* ok = static_cast<bool*>(f.raw(4));
        auto* lvl = f.ret<ue::UObject*>();
        if (!ok || !*ok || !ue::alive(lvl)) {
            for (auto& h : rec.levels) // nothing kept on failure
                if (auto* l = h.get()) {
                    static reflect::FuncRef rm = reflect::Resolver("LevelStreaming", reflect::find_class(L"/Script/Engine.LevelStreaming")).func(L"SetIsRequestingUnloadAndRemoval", 1);
                    if (rm.ok()) reflect::Frame(rm).arg_bool(0, true).invoke(l);
                }
            return "package not found: " + p.package;
        }
        rec.levels.push_back(ObjHandle::of(lvl));
    }
    g_maps.emplace(std::string(map_id), std::move(rec));
    log::info("world: background map {} streaming at Y {:+.0f}", map_id, offset_y);
    return {};
}

void unload(std::string_view map_id) {
    auto it = g_maps.find(map_id);
    if (it == g_maps.end()) return;
    static reflect::FuncRef rm = reflect::Resolver("LevelStreaming", reflect::find_class(L"/Script/Engine.LevelStreaming")).func(L"SetIsRequestingUnloadAndRemoval", 1);
    for (auto& h : it->second.levels)
        if (auto* l = h.get(); l && rm.ok()) reflect::Frame(rm).arg_bool(0, true).invoke(l);
    log::info("world: background map {} unloading", map_id);
    g_maps.erase(it);
}

bool loaded(std::string_view map_id) { return g_maps.find(map_id) != g_maps.end(); }

bool ready(std::string_view map_id) {
    auto it = g_maps.find(map_id);
    if (it == g_maps.end()) return false;
    static reflect::FuncRef is_loaded = reflect::Resolver("LevelStreaming", reflect::find_class(L"/Script/Engine.LevelStreaming")).func(L"IsLevelLoaded", 0);
    if (!is_loaded.ok()) return false;
    for (auto& h : it->second.levels) {
        auto* l = h.get();
        reflect::Frame f(is_loaded);
        if (!l || !f.invoke(l) || !f.ret_bool()) return false;
    }
    return true;
}

std::string map_of(const void* obj) {
    auto* o = static_cast<const ue::UObject*>(obj);
    if (!o || g_maps.empty()) return {};
    for (int depth = 0; o && depth < 16; ++depth) {
        auto* outer = o->GetOuterPrivate();
        if (!outer) { // the package: its name carries the instance name override
            const auto name = log::utf8(o->GetName());
            const auto at = name.find(kPrefix);
            if (at == std::string::npos) return {};
            auto rest = name.substr(at + kPrefix.size());
            const auto sep = rest.find("__");
            return sep == std::string::npos ? std::string{} : rest.substr(0, sep);
        }
        o = outer;
    }
    return {};
}

std::string describe() {
    std::string out = std::format("background maps: {}\n", g_maps.size());
    for (const auto& [id, m] : g_maps) out += std::format("  {} Y{:+.0f} parts={} ready={}\n", id, m.offset_y, m.levels.size(), ready(id));
    return out;
}

} // namespace elmp::world::level_instance
