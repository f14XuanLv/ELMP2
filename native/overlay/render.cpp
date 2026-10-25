#include "overlay/render.h"

#include "overlay/draw.h"
#include "overlay/model.h"

#include "input/rawinput.h"

#include "base/clock.h"
#include "base/hook.h"
#include "base/log.h"
#include "base/metrics.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace elmp::overlay {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_present{};
ResizeFn g_resize{};
bool g_installed = false;
std::atomic<bool> g_failed{false}, g_ready{false};
std::atomic<float> g_display_w{0}, g_display_h{0};
std::filesystem::path g_data;

struct Renderer {
    HWND hwnd{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    ID3D11RenderTargetView* rtv{};
    ImGuiContext* imgui{};
    ImFont* font{};
} g;
std::mutex g_render_mutex;

// Latest model copies (render thread only).
Hud g_hud;
Menu g_menu;
Manager g_manager;

metrics::Histogram& h_render = metrics::histogram("overlay.render_us");

// ---- mouse: window thread queues, render thread feeds ImGui ----
struct MouseEvent {
    UINT msg;
    int x, y, wheel, client_w, client_h;
};
std::mutex g_mouse_mutex;
std::vector<MouseEvent> g_mouse_events;
std::atomic<bool> g_wants_mouse{false};

void drain_mouse(ImGuiIO& io) {
    std::vector<MouseEvent> events;
    {
        std::scoped_lock lock(g_mouse_mutex);
        events.swap(g_mouse_events);
    }
    for (const auto& e : events) {
        const float sx = e.client_w > 0 ? io.DisplaySize.x / static_cast<float>(e.client_w) : 1.0f;
        const float sy = e.client_h > 0 ? io.DisplaySize.y / static_cast<float>(e.client_h) : 1.0f;
        switch (e.msg) {
        case WM_MOUSEMOVE: io.AddMousePosEvent(static_cast<float>(e.x) * sx, static_cast<float>(e.y) * sy); break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            io.AddMousePosEvent(static_cast<float>(e.x) * sx, static_cast<float>(e.y) * sy);
            io.AddMouseButtonEvent(0, true);
            break;
        case WM_LBUTTONUP: io.AddMouseButtonEvent(0, false); break;
        case WM_RBUTTONDOWN: io.AddMouseButtonEvent(1, true); break;
        case WM_RBUTTONUP: io.AddMouseButtonEvent(1, false); break;
        case WM_MOUSEWHEEL: io.AddMouseWheelEvent(0.0f, static_cast<float>(e.wheel) / static_cast<float>(WHEEL_DELTA)); break;
        default: break;
        }
    }
}

metrics::Counter& c_mouse = metrics::counter("overlay.mouse_events");
std::atomic<float> g_mouse_x{-1}, g_mouse_y{-1};
std::atomic<bool> g_hover{false};

std::atomic<std::int64_t> g_injected_until{0}; // Dev: injected test input owns the overlay mouse for a moment

bool mouse_hook(void* hwnd, unsigned msg, std::uintptr_t wparam, std::intptr_t lparam) {
    ++c_mouse;
    if (!g_ready || clock::now_us() < g_injected_until.load()) return false;
    RECT rc{};
    GetClientRect(static_cast<HWND>(hwnd), &rc);
    MouseEvent e{msg, static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)), 0, rc.right - rc.left, rc.bottom - rc.top};
    if (msg == WM_MOUSEWHEEL) e.wheel = GET_WHEEL_DELTA_WPARAM(wparam); // screen coordinates: position not used
    {
        std::scoped_lock lock(g_mouse_mutex);
        if (g_mouse_events.size() < 256) g_mouse_events.push_back(e);
    }
    return msg != WM_MOUSEMOVE && g_wants_mouse.load();
}

// ---- icons (render thread) ----
struct Icon {
    ID3D11ShaderResourceView* srv{};
    float w{}, h{};
};
std::unordered_map<std::string, Icon> g_icons; // failures cached too (srv null)

Icon load_icon(const std::filesystem::path& file) {
    Icon icon;
    IWICImagingFactory* factory{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICFormatConverter* converter{};
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT w{}, h{};
        converter->GetSize(&w, &h);
        if (w > 0 && h > 0 && w <= 1024 && h <= 1024) {
            std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
            if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = w;
                d.Height = h;
                d.MipLevels = d.ArraySize = 1;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_IMMUTABLE;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const D3D11_SUBRESOURCE_DATA data{px.data(), w * 4, 0};
                ID3D11Texture2D* tex{};
                if (SUCCEEDED(g.device->CreateTexture2D(&d, &data, &tex))) {
                    if (SUCCEEDED(g.device->CreateShaderResourceView(tex, nullptr, &icon.srv))) icon.w = static_cast<float>(w), icon.h = static_cast<float>(h);
                    tex->Release();
                }
            }
        }
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (com) CoUninitialize();
    return icon;
}

void release_icons() {
    for (auto& [id, i] : g_icons)
        if (i.srv) i.srv->Release();
    g_icons.clear();
}

void release_rtv() {
    if (g.rtv) g.rtv->Release();
    g.rtv = nullptr;
}

bool create_rtv(IDXGISwapChain* swap) {
    ID3D11Texture2D* back{};
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
    const HRESULT hr = g.device->CreateRenderTargetView(back, nullptr, &g.rtv);
    back->Release();
    return SUCCEEDED(hr);
}

// The game's composite font FOT-PearlStd-L_Font assigns sub-faces by character range (not by language):
// Pearl by default, chinese-traditional (FZFW ZhenZhuTi JF, simplified + traditional) for CJK, korean, russian.
ImFont* load_font(ImGuiIO& io) {
    static const ImWchar chinese[] = {0x3300, 0x33FF, 0xFE30, 0xFE4F, 0xF900, 0xFAFF, 0x2E80, 0x2EFF, 0x31C0, 0x31EF, 0x3000, 0x303F, 0x4E00, 0x9FFF, 0x3400, 0x4DBF, 0};
    static const ImWchar korean[] = {0x3130, 0x318F, 0x1100, 0x11FF, 0xAC00, 0xD7AF, 0};
    static const ImWchar russian[] = {0x0400, 0x04FF, 0x2DE0, 0x2DFF, 0xA640, 0xA69F, 0x1C80, 0x1C87, 0};
    static const ImWchar all_sub[] = {0x3300, 0x33FF, 0xFE30, 0xFE4F, 0xF900, 0xFAFF, 0x2E80, 0x2EFF, 0x31C0, 0x31EF, 0x3000, 0x303F, 0x4E00, 0x9FFF, 0x3400,
                                      0x4DBF, 0x3130, 0x318F, 0x1100, 0x11FF, 0xAC00, 0xD7AF, 0x0400, 0x04FF, 0x2DE0, 0x2DFF, 0xA640, 0xA69F, 0x1C80, 0x1C87, 0};
    const auto dir = g_data / "fonts";
    const auto primary = dir / "FOT-PearlStd-L.otf";
    ImFont* font = nullptr;
    const bool subs = std::filesystem::exists(dir / "chinese-traditional.ttf");
    if (std::filesystem::exists(primary)) {
        ImFontConfig cfg;
        if (subs) cfg.GlyphExcludeRanges = all_sub;
        font = io.Fonts->AddFontFromFileTTF(primary.string().c_str(), 18.0f, &cfg);
    }
    if (!font) {
        log::warn("overlay: game font missing ({}), using the ImGui default", primary.string());
        return io.Fonts->AddFontDefault();
    }
    struct Sub {
        const char* file;
        const ImWchar* ranges;
    };
    for (const auto& s : {Sub{"chinese-traditional.ttf", chinese}, Sub{"korean.ttf", korean}, Sub{"russian.ttf", russian}}) {
        if (!std::filesystem::exists(dir / s.file)) continue;
        ImFontConfig merge;
        merge.MergeMode = true;
        io.Fonts->AddFontFromFileTTF((dir / s.file).string().c_str(), 18.0f, &merge, s.ranges);
    }
    return font;
}

bool is_game_window(HWND hwnd) {
    wchar_t name[64]{};
    if (!hwnd || !GetClassNameW(hwnd, name, static_cast<int>(std::size(name)))) return false;
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId() && std::wstring_view(name) == L"UnrealWindow";
}

// Dark translucent panels with gold accents (same palette as the HUD), not the ImGui blue.
void apply_style() {
    auto& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = st.GrabRounding = st.TabRounding = 3.0f;
    st.WindowBorderSize = st.FrameBorderSize = 1.0f;
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    const auto rgba = [](int r, int gg, int b, int a = 255) { return ImVec4(r / 255.0f, gg / 255.0f, b / 255.0f, a / 255.0f); };
    const ImVec4 gold = rgba(240, 210, 130), gold_dim = rgba(200, 175, 110, 140);
    auto* c = st.Colors;
    c[ImGuiCol_Text] = rgba(235, 232, 240);
    c[ImGuiCol_TextDisabled] = rgba(140, 138, 150);
    c[ImGuiCol_WindowBg] = rgba(14, 13, 18, 238);
    c[ImGuiCol_PopupBg] = rgba(18, 16, 22, 245);
    c[ImGuiCol_Border] = rgba(200, 195, 210, 70);
    c[ImGuiCol_FrameBg] = rgba(30, 28, 36, 230);
    c[ImGuiCol_FrameBgHovered] = rgba(48, 42, 50, 240);
    c[ImGuiCol_FrameBgActive] = rgba(60, 52, 58, 250);
    c[ImGuiCol_TitleBg] = rgba(20, 18, 24, 245);
    c[ImGuiCol_TitleBgActive] = rgba(26, 23, 30, 250);
    c[ImGuiCol_TitleBgCollapsed] = rgba(20, 18, 24, 200);
    c[ImGuiCol_ScrollbarBg] = rgba(14, 13, 18, 160);
    c[ImGuiCol_ScrollbarGrab] = rgba(80, 74, 84, 220);
    c[ImGuiCol_ScrollbarGrabHovered] = gold_dim;
    c[ImGuiCol_ScrollbarGrabActive] = gold;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold_dim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_Button] = rgba(34, 31, 40, 235);
    c[ImGuiCol_ButtonHovered] = rgba(62, 54, 60, 245);
    c[ImGuiCol_ButtonActive] = rgba(90, 76, 66, 255);
    c[ImGuiCol_Header] = rgba(52, 46, 54, 220);
    c[ImGuiCol_HeaderHovered] = rgba(70, 60, 64, 235);
    c[ImGuiCol_HeaderActive] = rgba(96, 80, 66, 245);
    c[ImGuiCol_Separator] = rgba(200, 195, 210, 60);
    c[ImGuiCol_Tab] = rgba(28, 25, 32, 235);
    c[ImGuiCol_TabHovered] = rgba(70, 60, 64, 245);
    c[ImGuiCol_TabSelected] = rgba(64, 54, 52, 250);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_TableHeaderBg] = rgba(26, 23, 30, 245);
    c[ImGuiCol_TableBorderStrong] = rgba(200, 195, 210, 60);
    c[ImGuiCol_TableBorderLight] = rgba(200, 195, 210, 35);
    c[ImGuiCol_TableRowBg] = rgba(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgba(255, 255, 255, 10);
    c[ImGuiCol_NavCursor] = gold;
}

bool init(IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc)) || FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g.device)))) return false;
    g.device->GetImmediateContext(&g.context);
    g.hwnd = desc.OutputWindow;
    IMGUI_CHECKVERSION();
    g.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g.imgui);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // mouse events come from the window hook only
    g.font = load_font(io);
    apply_style();
    // No Win32 platform backend: it would read the OS cursor (GetCursorPos) whenever the game is focused;
    // the overlay's mouse comes only from the window hook (and Dev injection), keyboard / gamepad nav are unused.
    io.BackendPlatformName = "elmp";
    if (!ImGui_ImplDX11_Init(g.device, g.context) || !create_rtv(swap)) return false;
    log::info("overlay ready ({}x{})", desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

void render(IDXGISwapChain* swap) {
    if (g_failed) return;
    DXGI_SWAP_CHAIN_DESC check{};
    if (FAILED(swap->GetDesc(&check))) return;
    if (g_ready ? check.OutputWindow != g.hwnd : !is_game_window(check.OutputWindow)) return;
    if (!g_ready) {
        if (!init(swap)) {
            g_failed = true;
            log::error("overlay init failed; HUD and ELMP windows disabled");
            return;
        }
        g_ready = true;
    }
    if (!g.rtv && !create_rtv(swap)) return;
    const auto t0 = clock::now_us();
    hud().load(g_hud);
    menu().load(g_menu);
    manager().load(g_manager);
    ImGui::SetCurrentContext(g.imgui);
    ImGui_ImplDX11_NewFrame();
    auto& io = ImGui::GetIO();
    static std::int64_t last_us = 0;
    io.DeltaTime = last_us ? std::clamp(static_cast<float>(t0 - last_us) / 1e6f, 1e-4f, 0.25f) : 1.0f / 60.0f;
    last_us = t0;
    io.DisplaySize = ImVec2(static_cast<float>(check.BufferDesc.Width), static_cast<float>(check.BufferDesc.Height)); // back buffer, not window DPI
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    g_display_w = io.DisplaySize.x, g_display_h = io.DisplaySize.y;
    drain_mouse(io);
    ImGui::NewFrame();
    const double t = now();
    draw_hud(g_hud, g.font, t);
    draw_menu(g_menu, g.font, t);
    draw_manager(g_manager, t);
    g_wants_mouse = g_manager.open || io.WantCaptureMouse;
    g_mouse_x = io.MousePos.x, g_mouse_y = io.MousePos.y, g_hover = io.WantCaptureMouse;
    ImGui::Render();
    ID3D11RenderTargetView* old_rtv{};
    ID3D11DepthStencilView* old_dsv{};
    g.context->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    g.context->OMSetRenderTargets(1, &g.rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g.context->OMSetRenderTargets(1, &old_rtv, old_dsv);
    if (old_rtv) old_rtv->Release();
    if (old_dsv) old_dsv->Release();
    h_render.add(static_cast<float>(clock::now_us() - t0));
}

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    {
        std::scoped_lock lock(g_render_mutex);
        try {
            render(swap);
        } catch (...) {
            g_failed = true;
        }
    }
    return g_present(swap, sync, flags);
}

HRESULT STDMETHODCALLTYPE hooked_resize(IDXGISwapChain* swap, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    std::scoped_lock lock(g_render_mutex);
    DXGI_SWAP_CHAIN_DESC desc{};
    if (g_ready && SUCCEEDED(swap->GetDesc(&desc)) && desc.OutputWindow == g.hwnd) release_rtv();
    return g_resize(swap, count, w, h, format, flags);
}

// Probe device + swap chain only to read the vtable (Present = 8, ResizeBuffers = 13).
bool swapchain_vtable(void*& present, void*& resize) {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"ELMPOverlayProbe", nullptr};
    RegisterClassExW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &device, nullptr, &context);
    const bool ok = SUCCEEDED(hr) && swap;
    if (ok) {
        auto** vt = *reinterpret_cast<void***>(swap);
        present = vt[8];
        resize = vt[13];
    }
    if (swap) swap->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace

std::string install(const std::filesystem::path& data_dir) {
    if (g_installed) return {};
    g_data = data_dir;
    void *present{}, *resize{};
    if (!swapchain_vtable(present, resize)) return "cannot create a probe D3D11 swap chain";
    if (!hook::install_address("DXGI.Present", present, reinterpret_cast<void*>(&hooked_present), reinterpret_cast<void**>(&g_present), "system") ||
        !hook::install_address("DXGI.ResizeBuffers", resize, reinterpret_cast<void*>(&hooked_resize), reinterpret_cast<void**>(&g_resize), "system"))
        return "DXGI hooks refused";
    if (auto e = input::rawinput::install(); !e.empty()) log::warn("overlay: mouse needs the game window: {}", e);
    input::rawinput::set_mouse_hook(&mouse_hook);
    g_installed = true;
    return {};
}

bool installed() { return g_installed; }
bool ready() { return g_ready; }

void uninstall() {
    input::rawinput::set_mouse_hook(nullptr);
    std::scoped_lock lock(g_render_mutex);
    if (g_ready) {
        ImGui::SetCurrentContext(g.imgui);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(g.imgui);
    }
    release_rtv();
    release_icons();
    if (g.context) g.context->Release();
    if (g.device) g.device->Release();
    g = {};
    g_ready = false;
    g_installed = false;
}

double now() { return clock::now_s(); }

bool display_size(float& w, float& h) {
    w = g_display_w.load(), h = g_display_h.load();
    return w > 0 && h > 0;
}

#ifdef ELMP_DEV
void inject_mouse(int x, int y, bool click) {
    g_injected_until = clock::now_us() + 1'500'000; // the real cursor crossing the window must not move it meanwhile
    std::scoped_lock lock(g_mouse_mutex);           // client size 0 = coordinates are already back-buffer pixels
    g_mouse_events.push_back({WM_MOUSEMOVE, x, y, 0, 0, 0});
    if (!click) return;
    g_mouse_events.push_back({WM_LBUTTONDOWN, x, y, 0, 0, 0});
    g_mouse_events.push_back({WM_LBUTTONUP, x, y, 0, 0, 0});
}
#endif

std::string debug() {
    return std::format("ready={} failed={} mouse_events={} imgui_mouse=({:.0f},{:.0f}) over_elmp={} wants_mouse={}", g_ready.load(), g_failed.load(), c_mouse.load(),
                       g_mouse_x.load(), g_mouse_y.load(), g_hover.load(), g_wants_mouse.load());
}

unsigned long long icon_texture(const char* id, float& w, float& h) {
    if (!g.device || !id || !*id) return 0;
    std::string key(id);
    if (key.size() > 96 || !std::all_of(key.begin(), key.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; })) return 0;
    auto it = g_icons.find(key);
    if (it == g_icons.end()) it = g_icons.emplace(key, load_icon(g_data / "icons" / (key + ".png"))).first;
    w = it->second.w;
    h = it->second.h;
    return reinterpret_cast<unsigned long long>(it->second.srv);
}

} // namespace elmp::overlay
