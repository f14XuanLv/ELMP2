#pragma once

// SDK service (C20): mod registry from Mods/*/elmp.json, per-mod ElmpApi_v1 tables (C++ via the exported
// ELMP_GetApi), and the same table as Lua C functions ELMP_* for Lua mods with an elmp.json (no Lua
// source injected; events for Lua via ELMP_PollEvents → JSON array).

#include <filesystem>
#include <string>
#include <string_view>

struct ElmpApi_v1;
namespace RC::LuaMadeSimple { class Lua; }

namespace elmp::sdk {

void install(const std::filesystem::path& mods_root, const std::filesystem::path& storage_root);
const ElmpApi_v1* api_for(std::string_view mod_name, std::string* why = nullptr);
void on_lua_start(std::wstring_view mod_name, RC::LuaMadeSimple::Lua& lua, RC::LuaMadeSimple::Lua& main, RC::LuaMadeSimple::Lua& async,
                  RC::LuaMadeSimple::Lua* hook);
std::string describe();

} // namespace elmp::sdk
