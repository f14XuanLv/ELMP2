#include "engine/ue.h"

#include "base/log.h"

#include <Windows.h>

#include <atomic>

namespace elmp::ue {

using namespace RC::Unreal;

namespace {
std::atomic<DWORD> g_game_thread{0};
}

bool alive(const UObject* o) noexcept {
    if (!o) return false;
    auto* m = const_cast<UObject*>(o);
    return !m->IsUnreachable() && !m->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed));
}

std::wstring name_of(const UObject* o) { return o ? const_cast<UObject*>(o)->GetName() : std::wstring{}; }

std::wstring class_name_of(const UObject* o) {
    if (!o) return {};
    UClass* c = const_cast<UObject*>(o)->GetClassPrivate();
    return c ? c->GetName() : std::wstring{};
}

std::string utf8_name(const UObject* o) { return log::utf8(name_of(o)); }

std::string utf8(const FName& n) { return log::utf8(const_cast<FName&>(n).ToString()); }

bool on_game_thread() noexcept { return g_game_thread.load(std::memory_order_relaxed) == GetCurrentThreadId(); }

void mark_game_thread() noexcept { g_game_thread.store(GetCurrentThreadId(), std::memory_order_relaxed); }

} // namespace elmp::ue
