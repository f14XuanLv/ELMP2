#include "input/xinput.h"

#include "input/router.h"

#include "base/log.h"

#include <Windows.h>
#include <Xinput.h>

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

namespace elmp::input::xinput {

namespace {

constexpr const char* kDll = "XINPUT1_3.dll";
constexpr WORD kGetStateOrdinal = 2;

using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

GetState g_original = nullptr;
void** g_entry = nullptr;
std::atomic<DWORD> g_packet{1};
gamepad::State g_last{};

DWORD WINAPI hooked(DWORD index, XINPUT_STATE* state) {
    if (!router::active() || index >= static_cast<DWORD>(router::kPads) || !state) return g_original(index, state);
    if (index == 0) {
        if (const auto merged = router::player1_pad(&read)) {
            const auto& m = *merged;
            if (std::memcmp(&m, &g_last, sizeof(m)) != 0) {
                g_last = m;
                ++g_packet;
            }
            std::memset(state, 0, sizeof(*state));
            state->dwPacketNumber = g_packet;
            state->Gamepad = {m.buttons, m.left_trigger, m.right_trigger, m.thumb_lx, m.thumb_ly, m.thumb_rx, m.thumb_ry};
            return ERROR_SUCCESS;
        }
    }
    const DWORD r = g_original(index, state);
    if (r == ERROR_SUCCESS) std::memset(state, 0, sizeof(*state)); // owned by a seat / merged into 0: neutral
    return r;
}

void** find_entry(HMODULE module) {
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), kDll) != 0) continue;
        auto* lookup = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++iat)
            if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal) && IMAGE_ORDINAL(lookup->u1.Ordinal) == kGetStateOrdinal) return reinterpret_cast<void**>(&iat->u1.Function);
    }
    return nullptr;
}

bool write_entry(void** entry, void* value) {
    DWORD old{};
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *entry = value;
    VirtualProtect(entry, sizeof(void*), old, &old);
    return true;
}

GetState system_fn() {
    if (g_original) return g_original;
    auto* dll = GetModuleHandleA(kDll);
    return dll ? reinterpret_cast<GetState>(GetProcAddress(dll, MAKEINTRESOURCEA(kGetStateOrdinal))) : nullptr;
}

} // namespace

namespace {
// Probing an unplugged XInput index costs ~0.15 ms+ (measured on Dev2: 1 Hz probe of three empty indices
// pushed input.pump P99 to 463 µs). The probe therefore runs on its own thread; the router keeps atomics.
std::jthread g_probe;
} // namespace

std::string install() {
    if (!g_probe.joinable())
        g_probe = std::jthread([](std::stop_token st) {
            while (!st.stop_requested()) {
                refresh_connected();
                for (int i = 0; i < 10 && !st.stop_requested(); ++i) Sleep(100);
            }
        });
    if (g_entry) return {};
    auto* fn = system_fn();
    if (!fn) return std::format("{} XInputGetState not available", kDll);
    auto** entry = find_entry(GetModuleHandleW(nullptr));
    if (!entry) return "game does not import XInputGetState by ordinal";
    if (*entry != reinterpret_cast<void*>(fn)) return "XInputGetState import was already modified by another tool";
    g_original = fn;
    if (!write_entry(entry, reinterpret_cast<void*>(&hooked))) return "cannot write the import table entry";
    g_entry = entry;
    log::info("xinput: import table entry redirected");
    return {};
}

void uninstall() {
    if (g_probe.joinable()) {
        g_probe.request_stop();
        g_probe.join();
    }
    if (!g_entry) return;
    write_entry(g_entry, reinterpret_cast<void*>(g_original));
    g_entry = nullptr;
}

bool installed() { return g_entry != nullptr; }

#ifdef ELMP_DEV
namespace {
std::mutex g_fake_mutex; // the probe thread reads too
std::array<std::optional<gamepad::State>, router::kPads> g_fake;
} // namespace
void set_fake(int index, const std::optional<gamepad::State>& state) {
    {
        std::scoped_lock lock(g_fake_mutex);
        if (index >= 0 && index < router::kPads) g_fake[index] = state;
    }
    refresh_connected();
}
#endif

std::optional<gamepad::State> read(int index) {
    if (index < 0 || index >= router::kPads) return std::nullopt;
#ifdef ELMP_DEV
    {
        std::scoped_lock lock(g_fake_mutex);
        if (g_fake[index]) return g_fake[index];
    }
#endif
    auto* get = system_fn();
    XINPUT_STATE s{};
    if (!get || get(static_cast<DWORD>(index), &s) != ERROR_SUCCESS) return std::nullopt;
    const auto& p = s.Gamepad;
    return gamepad::State{p.wButtons, p.bLeftTrigger, p.bRightTrigger, p.sThumbLX, p.sThumbLY, p.sThumbRX, p.sThumbRY};
}

void refresh_connected() {
    for (int i = 0; i < router::kPads; ++i) router::set_connected(i, read(i).has_value());
}

} // namespace elmp::input::xinput
