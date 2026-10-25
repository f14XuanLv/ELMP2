// ElmpFrameProbe: Dev-only measurement mod for the G0 gate (frame time with ELMP vs. bare UE4SS).
// It is deployed alone (baseline) or next to ELMP and measures the same thing in both cases: the
// monotonic interval between consecutive UEngine::Tick pre-callbacks. No reflection, no allocation
// while sampling.
//
//   console: elmpprobe start <seconds> [label] [warmup_seconds]  → writes Mods/ElmpFrameProbe/out/<label>.json
//            elmpprobe status

#include "base/clock.h"
#include "base/json.h"
#include "base/log.h"
#include "base/metrics.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/FOutputDevice.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include <Windows.h>

#include <filesystem>
#include <format>
#include <sstream>
#include <vector>

namespace {

using namespace RC::Unreal;
using namespace elmp;

std::filesystem::path module_dir() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&module_dir), &module);
    wchar_t buf[MAX_PATH * 2]{};
    const auto n = GetModuleFileNameW(module, buf, static_cast<DWORD>(std::size(buf)));
    return n ? std::filesystem::path(buf).parent_path().parent_path() : std::filesystem::path{};
}

struct Session {
    std::string label;
    std::int64_t warm_until = 0, end = 0, last = 0;
    std::vector<float> samples;
    bool active = false;
    std::string last_result = "idle";
};

Session g;
std::filesystem::path g_dir;

// UE4SS maps the DLL of a mod disabled in mods.txt too, so "mapped" is not "running": ELMP exports
// elmp_started(), true once its mod object was started (preflight ran).
bool elmp_loaded() {
    HMODULE m = GetModuleHandleW((g_dir.parent_path() / "ELMP" / "dlls" / "main.dll").c_str());
    using Fn = bool (*)();
    auto fn = m ? reinterpret_cast<Fn>(GetProcAddress(m, "elmp_started")) : nullptr;
    return fn && fn();
}

void finish() {
    g.active = false;
    const auto s = metrics::summarize(g.samples);
    json::Value root = json::Value::object();
    root.set("schema", 1);
    root.set("label", g.label);
    root.set("elmpLoaded", elmp_loaded());
    root.set("samples", static_cast<std::int64_t>(s.n));
    root.set("meanMs", s.mean / 1000.0);
    root.set("p50Ms", s.p50 / 1000.0);
    root.set("p95Ms", s.p95 / 1000.0);
    root.set("p99Ms", s.p99 / 1000.0);
    root.set("maxMs", s.max / 1000.0);
    const auto file = g_dir / "out" / (log::wide(g.label) + L".json");
    std::string err;
    const bool ok = json_io::save(file, root, &err);
    g.last_result = std::format("{}: n={} mean={:.3f} p50={:.3f} p95={:.3f} p99={:.3f} max={:.3f} ms elmp={} -> {}", g.label, s.n, s.mean / 1000, s.p50 / 1000,
                                s.p95 / 1000, s.p99 / 1000, s.max / 1000, elmp_loaded(), ok ? file.string() : "write failed: " + err);
    RC::Output::send(STR("[ElmpFrameProbe] {}\n"), log::wide(g.last_result));
}

void on_tick() {
    const auto now = clock::now_us();
    if (g.active && g.last && now >= g.warm_until) g.samples.push_back(static_cast<float>(now - g.last));
    g.last = now;
    if (g.active && now >= g.end) finish();
}

bool command(const TCHAR* cmd, FOutputDevice& ar) {
    std::wistringstream in(cmd ? cmd : L"");
    std::wstring head, sub;
    in >> head >> sub;
    if (_wcsicmp(head.c_str(), L"elmpprobe") != 0) return false;
    std::string reply;
    if (sub == L"start") {
        double seconds = 15, warm = 1;
        std::wstring label = L"probe";
        in >> seconds;
        if (!(in >> label)) label = L"probe";
        in >> warm;
        const auto now = clock::now_us();
        g = Session{.label = log::utf8(label), .warm_until = now + static_cast<std::int64_t>(warm * 1e6),
                    .end = now + static_cast<std::int64_t>((warm + seconds) * 1e6), .last = 0, .active = true};
        g.samples.reserve(static_cast<std::size_t>(seconds * 400) + 64);
        reply = std::format("sampling {} s after {} s warm-up as '{}'", seconds, warm, g.label);
    } else {
        reply = g.active ? std::format("sampling '{}' ({} samples so far)", g.label, g.samples.size()) : g.last_result;
    }
    ar.Log(log::wide(reply).c_str());
    RC::Output::send(STR("[ElmpFrameProbe] {}\n"), log::wide(reply));
    return true;
}

class Probe final : public RC::CppUserModBase {
public:
    Probe() {
        ModName = STR("ElmpFrameProbe");
        ModVersion = STR("1");
        ModDescription = STR("ELMP Dev frame interval probe");
        ModAuthors = STR("ELMP");
        g_dir = module_dir();
    }
    ~Probe() override {
        for (auto id : hooks_) Hook::UnregisterCallback(id);
    }
    void on_unreal_init() override {
        Hook::FCallbackOptions opt;
        opt.OwnerModName = STR("ElmpFrameProbe");
        opt.HookName = STR("tick");
        hooks_.push_back(Hook::RegisterEngineTickPreCallback([](auto&, UEngine*, float, bool) { on_tick(); }, opt));
        opt.HookName = STR("console");
        hooks_.push_back(Hook::RegisterProcessConsoleExecCallback(
            [](auto& data, UObject*, const TCHAR* cmd, FOutputDevice& ar, UObject*) {
                if (!data.GetCurrentResolvedReturnValue() && command(cmd, ar)) data.TrySetReturnValue(true);
            },
            opt));
    }

private:
    std::vector<Hook::GlobalCallbackId> hooks_;
};

} // namespace

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod() { return new Probe(); }
extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
