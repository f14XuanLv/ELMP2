#pragma once

// PolyHook2 detours (X-5). Only two sources of addresses are accepted:
//   - a preflight-verified function from data/binary/EL.json (SHA-256 + prefix), or
//   - a reflected UFunction's native implementation (FR-PLT-4a), still requiring preflight ok.
// Links against PolyHook2 from the UE4SS target, so it is part of the mod, not of elmp_base.

#include <string>
#include <string_view>
#include <vector>

namespace elmp::hook {

struct Armed {
    std::string name;
    void* target = nullptr;
    std::string origin; // "manifest" | "reflect" | "system"
};

// Returns false (and logs) on any refusal; *original receives the trampoline on success.
bool install_manifest(std::string_view function_name, void* detour, void** original);
bool install_address(std::string_view name, void* target, void* detour, void** original, std::string_view origin);
bool installed(std::string_view name);
void uninstall_all();
std::vector<Armed> armed();

} // namespace elmp::hook
