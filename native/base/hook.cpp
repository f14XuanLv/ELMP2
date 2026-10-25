#include "base/hook.h"

#include "base/log.h"
#include "base/preflight.h"

#include <polyhook2/Detour/x64Detour.hpp>

#include <map>
#include <memory>

namespace elmp::hook {

namespace {

struct Entry {
    std::unique_ptr<PLH::x64Detour> detour;
    Armed info;
};

std::map<std::string, Entry, std::less<>>& table() {
    static std::map<std::string, Entry, std::less<>> t;
    return t;
}

} // namespace

bool install_address(std::string_view name, void* target, void* detour, void** original, std::string_view origin) {
    if (table().contains(name)) return true;
    if (!preflight::current().ok) {
        log::error("hook {} refused: preflight failed", name);
        return false;
    }
    if (!target) {
        log::error("hook {} refused: null target", name);
        return false;
    }
    auto d = std::make_unique<PLH::x64Detour>(reinterpret_cast<std::uint64_t>(target), reinterpret_cast<std::uint64_t>(detour),
                                              reinterpret_cast<std::uint64_t*>(original));
    if (!d->hook()) {
        log::error("hook {} failed: PolyHook2 refused", name);
        return false;
    }
    table().emplace(std::string(name), Entry{std::move(d), Armed{std::string(name), target, std::string(origin)}});
    log::info("hook armed: {} ({} @ {})", name, origin, target);
    return true;
}

bool install_manifest(std::string_view function_name, void* detour, void** original) {
    void* target = preflight::address(function_name);
    if (!target) {
        log::error("hook {} refused: not a verified manifest function", function_name);
        return false;
    }
    return install_address(function_name, target, detour, original, "manifest");
}

bool installed(std::string_view name) { return table().contains(name); }

void uninstall_all() {
    for (auto it = table().rbegin(); it != table().rend(); ++it) it->second.detour->unHook();
    table().clear();
}

std::vector<Armed> armed() {
    std::vector<Armed> out;
    for (const auto& [_, e] : table()) out.push_back(e.info);
    return out;
}

} // namespace elmp::hook
