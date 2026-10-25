// ELMP entry (CppUserModBase) and wiring. Preflight decides everything: on failure nothing is installed
// (F0.2, no degraded mode). On success: registry + lifecycle + console + frame pump; all per-frame work is
// scheduled by the pump.

#include "base/hook.h"
#include "base/log.h"
#include "base/preflight.h"
#include "base/settings.h"
#include "console/console.h"
#include "core/player.h"
#include "input/rawinput.h"
#include "input/xinput.h"
#include "party/config.h"
#include "party/devices.h"
#include "party/seat.h"
#include "core/permission.h"
#include "party/bench.h"
#include "party/progress.h"
#include "party/respawn.h"
#include "party/rules.h"
#include "party/transit.h"
#include "dev/lua_bridge.h"
#include "engine/binary_checks.h"
#include "engine/frame.h"
#include "engine/lifecycle.h"
#include "engine/registry.h"
#include "engine/world.h"

#include <elmp/build_info.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/CppUserModBase.hpp>

#include <Windows.h>

#include <filesystem>

namespace {

using namespace elmp;

std::filesystem::path module_dir() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&module_dir), &module);
    wchar_t buf[MAX_PATH * 2]{};
    const auto n = GetModuleFileNameW(module, buf, static_cast<DWORD>(std::size(buf)));
    return n ? std::filesystem::path(buf).parent_path() : std::filesystem::path{};
}

void sink(log::Level level, std::string_view text) {
    const auto w = log::wide(text);
    switch (level) {
    case log::Level::Error: RC::Output::send<RC::LogLevel::Error>(STR("[ELMP] {}\n"), w); break;
    case log::Level::Warning: RC::Output::send<RC::LogLevel::Warning>(STR("[ELMP] {}\n"), w); break;
    case log::Level::Verbose: RC::Output::send<RC::LogLevel::Verbose>(STR("[ELMP] {}\n"), w); break;
    default: RC::Output::send(STR("[ELMP] {}\n"), w); break;
    }
}

// Game windows are DPI-unaware by default; Dev launches set ELMP_DPI_AWARE=1 so the 1280x720 client
// area is not bitmap-stretched (v1 core-facts「实例启动」).
void dev_dpi_awareness() {
#ifdef ELMP_DEV
    wchar_t v[8]{};
    if (GetEnvironmentVariableW(L"ELMP_DPI_AWARE", v, static_cast<DWORD>(std::size(v))) && std::wstring_view(v) == L"1")
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
}

bool g_settings_loaded = false;

// Work that needs a live engine (saved dir via ProcessEvent, offset checks via reflection); retried
// on the frame pump until it succeeds, then the phase disables itself.
void deferred_init(const frame::Tick&) {
    const auto saved = world::saved_dir();
    if (saved.empty()) return;
    const auto file = std::filesystem::path(saved) / L"ELMP" / L"ELMP" / L"settings.json";
    if (const auto err = settings::load(file); !err.empty()) log::warn("settings {}: {}", file.string(), err);
    binary_checks::run();
    g_settings_loaded = true;
    log::info("saved dir {}", log::utf8(saved));
}

void define_settings() {
    settings::define({.key = "verboseLog", .def = false, .help = "extra diagnostics in UE4SS.log"});
    party::config::define();
}

class ElmpMod final : public RC::CppUserModBase {
public:
    ElmpMod() {
        ModName = STR("ELMP");
        ModVersion = log::wide(build::version);
        ModDescription = STR("EnderLilies Multiplayer");
        ModAuthors = STR("ELMP");
        ModIntendedSDKVersion = STR("3.0");
        log::set_sink(sink);
        dev_dpi_awareness();
        mod_dir_ = module_dir().parent_path(); // dlls/main.dll → Mods/ELMP
        define_settings();
    }

    ~ElmpMod() override {
        frame::uninstall();
        console::uninstall();
        lifecycle::uninstall();
        registry::uninstall();
        input::rawinput::uninstall();
        input::xinput::uninstall();
        hook::uninstall_all();
        log::set_sink(nullptr);
    }

    void on_unreal_init() override {
        const auto& pf = preflight::run(mod_dir_ / "data" / "binary" / "EL.json");
        if (!pf.ok) {
            log::error("{} preflight failed: {} (sha256 {}); ELMP disabled", build::version, pf.reason, pf.sha256);
            return;
        }
        log::info("{} ({}) preflight ok: sha256 {}, {} functions verified in {:.0f} ms", build::version, build::dev ? "dev" : "shipping", pf.sha256,
                  pf.functions.size(), pf.ms);
        registry::install();
        lifecycle::install();
        console::install();
        console::register_p0_commands();
        core::install();
        party::install();
        party::devices::install();
        core::permission::install();
        party::rules::install();
        party::bench::install();
        party::respawn::install();
        party::progress::install();
        party::transit::install();
        console::register_core_commands();
        console::register_party_commands();
        frame::set_activity([] { return core::active() > 0; }, nullptr);
        frame::add_phase("lifecycle", [](const frame::Tick& t) { lifecycle::step(t.now_us); });
        frame::add_phase("init", deferred_init, [] { return !g_settings_loaded; }, 250);
        frame::add_phase("registry.drain", [](const frame::Tick&) { registry::drain(); });
        frame::add_phase("registry.audit", [](const frame::Tick&) { registry::audit(); }, [] { return lifecycle::state() == lifecycle::State::Settled; }, 1000);
        frame::install();
        active_ = true;
    }

    void on_lua_start(RC::StringViewType mod_name, RC::LuaMadeSimple::Lua& lua, RC::LuaMadeSimple::Lua& main, RC::LuaMadeSimple::Lua& async,
                      RC::LuaMadeSimple::Lua* hook) override {
        if (active_) dev::on_lua_start(mod_name, lua, main, async, hook);
    }

private:
    std::filesystem::path mod_dir_;
    bool active_ = false;
};

} // namespace

namespace {
bool g_started = false;
}

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod() {
    g_started = true;
    return new ElmpMod();
}

// Dev tooling (ElmpFrameProbe) uses this to tell "ELMP running" from "DLL mapped but disabled".
extern "C" __declspec(dllexport) bool elmp_started() { return g_started; }

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
