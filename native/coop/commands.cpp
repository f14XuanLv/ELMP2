#include "coop/commands.h"

#include "engine/reflect.h"

#include "base/hook.h"
#include "base/log.h"

#include <format>
#include <unordered_set>
#include <unordered_map>

namespace elmp::coop::commands {

namespace {

using RC::Unreal::UObject;

struct Layout {
    reflect::PropRef result, action, input; // UCommand.CurrentResult / CommandAction / CommandInputType
    reflect::FuncRef dir;                   // UCommandAction::GetInvokedByDirectionInputType
    bool ok = false;
};

Layout g_l;
bool g_l_tried = false;

const Layout& layout() {
    if (g_l_tried) return g_l;
    g_l_tried = true;
    reflect::Resolver c("Command", reflect::find_class(L"/Script/Zenith.Command"));
    g_l.result = c.prop(L"CurrentResult", reflect::Kind::Any, 1);
    g_l.action = c.prop(L"CommandAction", reflect::Kind::Object);
    g_l.input = c.prop(L"CommandInputType", reflect::Kind::Any, 1);
    reflect::Resolver a("CommandAction", reflect::find_class(L"/Script/Zenith.CommandAction"));
    g_l.dir = a.func(L"GetInvokedByDirectionInputType", 0);
    g_l.ok = c.ok() && a.ok();
    if (!g_l.ok) log::warn("commands: layout: {} {}", c.failure(), a.failure());
    return g_l;
}

// char UCommandQueueComponent::ProcessCommand(this, ?, UCommand*) — 0xCB4F00（EL.json CommandProcessStart）
using ProcessFn = char(__fastcall*)(UObject*, void*, UObject*);
ProcessFn o_process{};
void (*g_sink)(const Start&) = nullptr;
bool g_installed = false, g_failed = false;
std::uint64_t g_captured = 0, g_replayed = 0, g_rejected = 0;

std::string class_path(UObject* cls) {
    static std::unordered_map<UObject*, std::pair<ObjHandle, std::string>> paths;
    auto& e = paths[cls];
    if (e.first.get() != cls) e = {ObjHandle::of(cls), log::utf8(cls->GetPathName())};
    return e.second;
}

char __fastcall d_process(UObject* queue, void* a2, UObject* command) {
    if (g_sink && queue && command && ue::on_game_thread() && layout().ok && *g_l.result.at<std::uint8_t>(command) == 0) {
        if (auto* action = g_l.action.object(command); action && action->GetClassPrivate() && queue->GetOuterPrivate()) {
            Start s;
            s.owner = queue->GetOuterPrivate();
            s.cls = action->GetClassPrivate();
            s.path = class_path(s.cls);
            s.cmd = *g_l.input.at<std::uint8_t>(command);
            reflect::Frame f(g_l.dir);
            if (f.invoke(action)) s.dir = f.ret<std::uint8_t>();
            ++g_captured;
            g_sink(s);
        }
    }
    return o_process(queue, a2, command);
}

// int32 UCommandQueueComponent::TryAddCommands(this, const FInputSnapshot&, const UCommandSet*) — 0xCC2520
// 由 Pawn Tick 调用；客户端跑模拟时必须抑制本地输入生成命令（命令只来自 Host 的 Cmd）。
using TryAddFn = std::int32_t(__fastcall*)(UObject*, const void*, const void*);
TryAddFn o_try_add{};
bool g_try_ok = false, g_try_tried = false;
std::unordered_set<UObject*> g_suppressed;
std::uint64_t g_suppressed_calls = 0;

std::int32_t __fastcall d_try_add(UObject* queue, const void* snapshot, const void* set) {
    if (!g_suppressed.empty() && queue && g_suppressed.count(queue->GetOuterPrivate())) return ++g_suppressed_calls, -1;
    return o_try_add(queue, snapshot, set);
}

ue::UObject* component_of(ue::UObject* actor, const wchar_t* cls_path) {    static reflect::FuncRef by_class = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"GetComponentByClass", 1);
    auto* cls = reflect::find_class(cls_path);
    if (!by_class.ok() || !cls || !actor) return nullptr;
    reflect::Frame f(by_class);
    f.arg(0, static_cast<ue::UObject*>(cls));
    return f.invoke(actor) ? f.ret<ue::UObject*>() : nullptr;
}

} // namespace

bool host_enable(void (*sink)(const Start&)) {
    if (!g_installed && !g_failed) {
        g_installed = hook::install_manifest("CommandProcessStart", reinterpret_cast<void*>(&d_process), reinterpret_cast<void**>(&o_process));
        g_failed = !g_installed;
    }
    g_sink = g_installed ? sink : nullptr;
    return g_installed;
}

void host_disable() { g_sink = nullptr; }

bool replay(ue::UObject* enemy, const std::string& cls_path, std::uint8_t dir, std::uint8_t cmd, std::string* why) {
    auto fail = [&](const char* w) {
        ++g_rejected;
        if (why) *why = w;
        return false;
    };
    if (!ue::alive(enemy)) return fail("no enemy");
    auto* cq = component_of(enemy, L"/Script/Zenith.CommandQueueComponent");
    if (!ue::alive(cq)) return fail("no CommandQueueComponent");
    auto* cls = reflect::load_object(log::wide(cls_path));
    if (!ue::alive(cls)) return fail("class not loaded");
    static reflect::FuncRef add = reflect::Resolver("CommandQueueComponent", reflect::find_class(L"/Script/Zenith.CommandQueueComponent")).func(L"AddNewImmediateCommand", 3);
    if (!add.ok()) return fail("AddNewImmediateCommand");
    reflect::Frame f(add);
    f.arg(0, static_cast<std::uint8_t>(dir)).arg(1, static_cast<std::uint8_t>(cmd)).arg(2, static_cast<ue::UObject*>(cls));
    if (!f.invoke(cq)) return fail("invoke");
    ++g_replayed;
    return true;
}

bool suppress(ue::UObject* enemy, bool on) {
    if (!g_try_tried) {
        g_try_ok = hook::install_manifest("CommandTryAddFromInput", reinterpret_cast<void*>(&d_try_add), reinterpret_cast<void**>(&o_try_add));
        g_try_tried = true;
    }
    if (!g_try_ok || !ue::alive(enemy)) return false;
    if (on) g_suppressed.insert(enemy);
    else g_suppressed.erase(enemy);
    return true;
}

void clear_suppressed() { g_suppressed.clear(); }

std::string describe() {
    return std::format("cmds: hook={} captured={} replayed={} rejected={} tryAdd={} suppressed={} calls={}\n", g_installed ? "armed" : "off", g_captured, g_replayed, g_rejected, g_try_ok ? "armed" : "off", g_suppressed.size(), g_suppressed_calls);
}

} // namespace elmp::coop::commands
