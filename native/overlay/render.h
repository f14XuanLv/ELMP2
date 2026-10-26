#pragma once

// In-game ImGui overlay on D3D11 (M23, F1.7 / F1.10 / F1.17). IDXGISwapChain::Present / ResizeBuffers are
// taken from a probe swap chain's vtable (8 / 13) and detoured (PolyHook2, preflight required); drawing
// happens before the vanilla Present, only on this process's UnrealWindow swap chain. ImGui is the static
// copy linked into main.dll (separate context from the UE4SS debug GUI). The render thread only reads the
// published models (overlay/model.h) and queues Requests; it never touches UObjects.
// Mouse: the game window procedure (input/rawinput) forwards WM_MOUSE* here; a click is withheld from the
// game only while an ELMP window wants the mouse (manager open or the cursor over an ELMP widget).

#include <cstdint>
#include <filesystem>
#include <string>

namespace elmp::overlay {

// data_dir = Mods/ELMP/data (icons/, fonts/). Game thread; idempotent. Empty = ok.
std::string install(const std::filesystem::path& data_dir);
void uninstall();
bool installed();
void set_banner(std::string text); // top-centre status line (empty = none); any thread
bool ready(); // renderer initialised on the game swap chain

double now(); // monotonic seconds (same clock as base/clock)
bool display_size(float& w, float& h); // back buffer of the game swap chain (last rendered frame)
std::string debug(); // mouse / ImGui state for `elmp manager`

#ifdef ELMP_DEV
// Dev test seam: feeds a mouse move (+ click) in back-buffer pixels straight into the overlay's own event
// queue — no window message, no OS cursor, nothing reaches the desktop. Register uses as a test aid.
void inject_mouse(int x, int y, bool click);
#endif

// Render-thread helpers for the draw code.
unsigned long long icon_texture(const char* id, float& width, float& height); // 0 = missing

} // namespace elmp::overlay
