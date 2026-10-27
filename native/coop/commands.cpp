#include "coop/commands.h"

#include "engine/reflect.h"

#include "base/hook.h"
#include "base/log.h"

#include <format>
#include <unordered_map>
#include <unordered_set>

namespace elmp::coop::commands {

namespace {

using RC::Unreal::UObject;

struct Layout {
    reflect::PropRef result, action, input; // UCommand.CurrentResult / CommandAction / CommandInputType
    reflect::FuncRef dir;                   // UCommandAction::GetInvokedByDirectionInputType
    bool ok = false, tried = false;
};
Layout g_l;

const Layout& layout() {
    if (g_l.tried) return g_l;
    g_l.tried = true;
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

// char UCommandQueueComponent::ProcessCommand(this, ?, UCommand*) — 0xCB4F00 (engine-facts「CommandQueue 决策管线」)
using ProcessFn = char(__fastcall*)(UObject*, void*, UObject*);
ProcessFn o_process{};
void (*g_sink)(const Start&) = nullptr;
bool g_installed = false, g_failed = false;
std::uint64_t g_captured = 0, g_replayed = 0, g_rejected = 0;

char __fastcall d_process(UObject* queue, void* a2, UObject* command) {
    if (g_sink && queue && command && ue::on_game_thread() && layout().ok && *g_l.result.at<std::uint8_t>(command) == 0) {
        auto* action = g_l.action.object(command);
        auto* cls = action ? action->GetClassPrivate() : nullptr;
        if (cls) {
            Start s;
            s.owner = queue->GetOuterPrivate();
            s.cls = cls;
            s.cmd = *g_l.input.at<std::uint8_t>(command);
            reflect::Frame f(g_l.dir);
            if (f.invoke(action)) s.dir = f.ret<std::uint8_t>();
            // path per class cached; entry valid while the weak handle still resolves to the same class
            static std::unordered_map<UObject*, std::pair<ObjHandle, std::string>> paths;
            auto& pe = paths[cls];
            if (pe.first.get() != cls) pe = {ObjHandle::of(cls), log::utf8(cls->GetPathName())};
            s.path = pe.second;
            ++g_captured;
            g_sink(s);
        }
    }
    return o_process(queue, a2, command);
}

// int32 UCommandQueueComponent::TryAddCommands(this, const FInputSnapshot&, const UCommandSet*) — 0xCC2520,
// called by the pawn ticks (ZenithCharacter::Tick 0xCF3740, 0xCEF190) with the consumed input snapshot
using TryAddFn = std::int32_t(__fastcall*)(UObject*, const void*, const void*);
TryAddFn o_try_add{};
bool g_try_installed = false, g_try_failed = false;
std::unordered_set<UObject*> g_suppressed; // pawns whose queue ignores input-made commands
std::uint64_t g_suppressed_calls = 0;

std::int32_t __fastcall d_try_add(UObject* queue, const void* snapshot, const void* set) {
    if (!g_suppressed.empty() && queue && g_suppressed.count(queue->GetOuterPrivate())) return ++g_suppressed_calls, -1;
    return o_try_add(queue, snapshot, set);
}

} // namespace

bool suppress_input_commands(ue::UObject* pawn, bool on) {
    if (!g_try_installed && !g_try_failed) {
        g_try_installed = hook::install_manifest("CommandTryAddFromInput", reinterpret_cast<void*>(&d_try_add), reinterpret_cast<void**>(&o_try_add));
        g_try_failed = !g_try_installed;
    }
    if (!g_try_installed || !pawn) return false;
    if (on) g_suppressed.insert(pawn);
    else g_suppressed.erase(pawn);
    return true;
}
void clear_suppressed() { g_suppressed.clear(); }

bool host_enable(void (*sink)(const Start&)) {
    if (!g_installed && !g_failed) {
        g_installed = hook::install_manifest("CommandProcessStart", reinterpret_cast<void*>(&d_process), reinterpret_cast<void**>(&o_process));
        g_failed = !g_installed;
    }
    g_sink = g_installed ? sink : nullptr;
    return g_installed;
}

void host_disable() { g_sink = nullptr; }

bool replay(ue::UObject* owner, const std::string& cls_path, std::uint8_t dir, std::uint8_t cmd, std::string* why) {
    auto fail = [&](const char* w) {
        ++g_rejected;
        if (why) *why = w;
        return false;
    };
    if (cmd > 8) return fail("command type not replayed"); // INTERACT / MAINMENU / PAUSE / MINIMAP
    static ue::UClass* action_base = reflect::find_class(L"/Script/Zenith.CommandAction");
    // class lookup by path is a StaticLoadObject string resolve → cache per path with a weak handle
    static std::unordered_map<std::string, ObjHandle> cache;
    auto& slot = cache[cls_path];
    auto* cls = static_cast<ue::UClass*>(slot.get());
    if (!cls) {
        cls = static_cast<ue::UClass*>(reflect::load_object(log::wide(cls_path)));
        slot = ObjHandle::of(cls);
    }
    if (!cls || !action_base || !cls->IsChildOf(action_base)) return fail("not a CommandAction class");
    static ObjHandle queue_cls = ObjHandle::of(reflect::find_object(L"/Script/Zenith.CommandQueueComponent"));
    static reflect::FuncRef by_class, add;
    if (!add.ok()) {
        by_class = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"GetComponentByClass", 1);
        add = reflect::Resolver("CommandQueueComponent", reflect::find_class(L"/Script/Zenith.CommandQueueComponent")).func(L"AddNewImmediateCommand", 3);
    }
    if (!add.ok() || !by_class.ok()) return fail("AddNewImmediateCommand");
    reflect::Frame c(by_class);
    c.arg(0, queue_cls.get());
    auto* queue = c.invoke(owner) ? c.ret<ue::UObject*>() : nullptr;
    if (!ue::alive(queue)) return fail("no CommandQueueComponent");
    reflect::Frame f(add);
    f.arg(0, dir).arg(1, cmd).arg(2, cls);
    if (!f.invoke(queue)) return fail("invoke");
    if (f.ret<std::int32_t>() < 0) return fail("rejected by the queue");
    ++g_replayed;
    return true;
}

std::string describe() {
    return std::format("commands: hook={} captured={} replayed={} rejected={} | inputSuppress={} copies={} suppressed={}\n", g_installed ? "armed" : (g_failed ? "failed" : "off"),
                       g_captured, g_replayed, g_rejected, g_try_installed ? "armed" : (g_try_failed ? "failed" : "off"), g_suppressed.size(), g_suppressed_calls);
}

} // namespace elmp::coop::commands
