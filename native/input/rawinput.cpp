#include "input/rawinput.h"

#include "input/router.h"
#include "input/xinput.h"

#include "base/log.h"

#include <Windows.h>
// clang-format off
#include <SetupAPI.h>
#include <hidsdi.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <format>
#include <map>
#include <mutex>

namespace elmp::input::rawinput {

namespace {

HWND g_hwnd = nullptr;
WNDPROC g_original = nullptr;
std::atomic<bool> g_registered{false};
bool g_swallow_char = false;
std::mutex g_ids_mutex;
std::map<HANDLE, std::string> g_ids;
std::atomic<bool> g_capturing{false};
std::atomic<std::uint32_t> g_captured{0};
std::atomic<MouseHook> g_mouse{nullptr};
std::atomic<bool> g_observe{false};

constexpr GUID kKeyboardInterface = {0x884b96c3, 0x56ef, 0x11d1, {0xbc, 0x8c, 0x00, 0xa0, 0xc9, 0x14, 0x05, 0xdd}}; // GUID_DEVINTERFACE_KEYBOARD

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring device_path(HANDLE device) {
    UINT size = 0;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size) != 0 || size == 0) return {};
    std::wstring name(size, L'\0');
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &size) == static_cast<UINT>(-1)) return {};
    name.resize(wcslen(name.c_str()));
    return lower(name);
}

// One physical keyboard often registers several interfaces (main keys / media keys / NKRO): merge them by
// VID&PID; keyboards without one (laptop PS/2) keep the full path (v1 rawinput).
std::string id_for_path(const std::wstring& path) {
    const auto vid = path.find(L"vid_"), pid = path.find(L"pid_");
    if (vid != std::wstring::npos && pid != std::wstring::npos && vid + 8 <= path.size() && pid + 8 <= path.size())
        return "kbd:" + log::utf8(path.substr(vid, 8)) + "&" + log::utf8(path.substr(pid, 8));
    return "kbd:" + log::utf8(path);
}

std::string id_of(HANDLE device) {
    std::scoped_lock lock(g_ids_mutex);
    if (const auto it = g_ids.find(device); it != g_ids.end()) return it->second;
    const auto path = device_path(device);
    auto id = path.empty() ? std::format("kbd:handle-{:x}", reinterpret_cast<std::uintptr_t>(device)) : id_for_path(path);
    g_ids.emplace(device, id);
    return id;
}

std::map<std::wstring, std::string> keyboard_names() {
    std::map<std::wstring, std::string> out;
    HDEVINFO set = SetupDiGetClassDevsW(&kKeyboardInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &kKeyboardInterface, i, &iface); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        if (!size) continue;
        std::vector<std::byte> buffer(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info{sizeof(info)};
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, &info)) continue;
        wchar_t name[256]{};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<PBYTE>(name), sizeof(name), nullptr) &&
            !SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<PBYTE>(name), sizeof(name), nullptr))
            continue;
        out[lower(detail->DevicePath)] = log::utf8(name);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

std::string hid_product(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    wchar_t product[128]{};
    const bool ok = HidD_GetProductString(h, product, sizeof(product));
    CloseHandle(h);
    return ok ? log::utf8(product) : std::string();
}

void register_keyboard(HWND hwnd) {
    RAWINPUTDEVICE rid{0x01, 0x06, 0, hwnd}; // Generic Desktop / Keyboard
    g_registered = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
    log::info("rawinput: keyboard registration {}", g_registered ? "ok" : "failed");
}

void on_raw_input(LPARAM lparam) {
    UINT size = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0 || size > 1024) return;
    alignas(8) std::byte buffer[1024];
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
    const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer);
    if (raw->header.dwType != RIM_TYPEKEYBOARD) return;
    const auto& kb = raw->data.keyboard;
    if (kb.VKey == 0 || kb.VKey >= 255) return; // 255 = synthetic
    const bool down = (kb.Flags & RI_KEY_BREAK) == 0;
    router::raw_key(id_of(raw->header.hDevice), kb.VKey, down);
    if (down && g_capturing.exchange(false)) g_captured = kb.VKey;
}

LRESULT CALLBACK hooked(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
        if (auto* m = g_mouse.load(std::memory_order_acquire); m && m(hwnd, msg, wparam, lparam)) return 0;
    if (!router::active() && !g_capturing && !g_observe) return CallWindowProcW(g_original, hwnd, msg, wparam, lparam);
    if (!g_registered) register_keyboard(hwnd);
    switch (msg) {
    case WM_INPUT: on_raw_input(lparam); break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (!router::legacy_allowed(static_cast<std::uint32_t>(wparam), down)) {
            g_swallow_char = down;
            return 0;
        }
        g_swallow_char = false;
        break;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        if (g_swallow_char) {
            g_swallow_char = false;
            return 0;
        }
        break;
    case WM_KILLFOCUS: router::clear_keys(); break;
    case WM_ACTIVATEAPP:
        if (!wparam) router::clear_keys();
        break;
    default: break;
    }
    return CallWindowProcW(g_original, hwnd, msg, wparam, lparam);
}

HWND find_game_window() {
    struct Search {
        DWORD pid;
        HWND found;
    } s{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM p) -> BOOL {
            auto* q = reinterpret_cast<Search*>(p);
            DWORD pid{};
            GetWindowThreadProcessId(hwnd, &pid);
            wchar_t name[64]{};
            if (pid == q->pid && GetClassNameW(hwnd, name, 64) && std::wstring_view(name) == L"UnrealWindow") {
                q->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.found;
}

} // namespace

std::string install() {
    if (g_hwnd) return {};
    HWND hwnd = find_game_window();
    if (!hwnd) return "game window not found";
    auto* previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hooked)));
    if (!previous) return "cannot subclass the game window";
    g_original = previous;
    g_hwnd = hwnd;
    PostMessageW(hwnd, WM_NULL, 0, 0);
    log::info("rawinput: game window subclassed");
    return {};
}

void uninstall() {
    if (!g_hwnd) return;
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC)) == &hooked) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original));
    g_hwnd = nullptr;
    g_original = nullptr;
}

bool installed() { return g_hwnd != nullptr; }
bool keyboard_registered() { return g_registered; }

std::vector<Device> devices() {
    std::vector<Device> out;
    UINT count = 0;
    GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (count && GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) != static_cast<UINT>(-1)) {
        const auto names = keyboard_names();
        int n = 0;
        for (const auto& d : list) {
            if (d.dwType != RIM_TYPEKEYBOARD) continue;
            const auto id = id_of(d.hDevice);
            if (std::any_of(out.begin(), out.end(), [&](const Device& e) { return e.id == id; })) continue;
            const auto path = device_path(d.hDevice);
            auto name = hid_product(path);
            if (name.empty())
                if (const auto it = names.find(path); it != names.end()) name = it->second;
            if (name.empty()) name = "Keyboard";
            out.push_back({id, "keyboard", std::format("{} #{}", name, ++n), true});
        }
    }
    for (int i = 0; i < router::kPads; ++i) out.push_back({router::pad_id(i), "gamepad", std::format("Gamepad {} (XInput)", i + 1), xinput::read(i).has_value()});
    return out;
}

#ifdef ELMP_DEV
std::string post_key(const std::string& keyboard, std::uint32_t vk, bool down) {
    if (!g_hwnd) return "window not subclassed";
    if (router::active()) router::raw_key(keyboard, vk, down);
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    const LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16) | (down ? 0 : (LPARAM(1) << 30) | (LPARAM(1) << 31));
    return PostMessageW(g_hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, lp) ? std::string{} : "PostMessage failed";
}
#endif

void set_mouse_hook(MouseHook hook) { g_mouse.store(hook, std::memory_order_release); }

void set_observe(bool on) {
    g_observe = on;
    if (on && g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0); // registers Raw Input on the window thread
}

void begin_capture() {
    g_captured = 0;
    g_capturing = true;
}

std::uint32_t take_captured_key() { return g_captured.exchange(0); }

} // namespace elmp::input::rawinput
