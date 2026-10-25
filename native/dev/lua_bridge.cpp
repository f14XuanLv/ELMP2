#include "dev/lua_bridge.h"

#include "console/console.h"
#include "engine/ue.h"

#include "base/log.h"

#include <LuaMadeSimple/LuaMadeSimple.hpp>

namespace elmp::dev {

using RC::LuaMadeSimple::Lua;

#ifdef ELMP_DEV
namespace {

// ELMP_Command("elmp status") -> output text. Game thread only (UEBridge eval runs there).
int command(const Lua& lua) {
    if (!lua.is_string()) {
        lua.set_string("ELMP_Command expects a string");
        return 1;
    }
    std::string line(lua.get_string());
    if (!ue::on_game_thread()) {
        lua.set_string("ELMP_Command refused: not on the game thread");
        return 1;
    }
    std::string out;
    if (!console::run(line, out)) out = "not an elmp command";
    lua.set_string(out);
    return 1;
}

} // namespace
#endif

void on_lua_start(std::wstring_view mod_name, Lua& lua, Lua& main, Lua& async, Lua* hook) {
#ifdef ELMP_DEV
    if (mod_name != L"UEBridge") return;
    for (Lua* state : {&lua, &main, &async, hook}) {
        if (state) state->register_function("ELMP_Command", command);
    }
    log::info("dev API injected into Lua mod 'UEBridge' (ELMP_Command)");
#else
    (void)mod_name, (void)lua, (void)main, (void)async, (void)hook;
#endif
}

} // namespace elmp::dev
