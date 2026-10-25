#pragma once

// Lua injection point (§11 / §12). Dev builds inject C functions (no Lua source) into the UEBridge
// state so the pilot can drive and observe ELMP; Shipping builds inject nothing in P0 (the SDK C
// bindings for declared third-party Lua mods arrive in P4). ELMP itself never depends on Lua.

#include <string_view>

namespace RC::LuaMadeSimple {
class Lua;
}

namespace elmp::dev {

void on_lua_start(std::wstring_view mod_name, RC::LuaMadeSimple::Lua& lua, RC::LuaMadeSimple::Lua& main, RC::LuaMadeSimple::Lua& async,
                  RC::LuaMadeSimple::Lua* hook);

} // namespace elmp::dev
