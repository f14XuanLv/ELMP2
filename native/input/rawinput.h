#pragma once

// Game window wiring (M7): the UnrealWindow procedure is subclassed and keyboard Raw Input is registered
// on the window thread (keyboard only; the game's own mouse registration is untouched).
//   - WM_INPUT (keyboard): per-device key state → router; always passed on to the game;
//   - WM_KEYDOWN/UP, WM_SYS*: dropped when the source keyboard's owner is not P1 (the following WM_CHAR too);
//   - focus loss clears every keyboard state.
// While the router is inactive the procedure forwards everything untouched (G0).

#include <cstdint>
#include <string>
#include <vector>

namespace elmp::input::rawinput {

struct Device {
    std::string id;   // kbd:<vid&pid | path>, xinput:N
    std::string kind; // keyboard | gamepad
    std::string name;
    bool connected = false;
};

std::string install(); // game thread; finds this process's UnrealWindow (idempotent)
void uninstall();
bool installed();
bool keyboard_registered();

std::vector<Device> devices(); // Raw Input keyboards (merged per physical device) + XInput 0..3

#ifdef ELMP_DEV
// Dev test seam: records a Raw Input event for `keyboard` and posts the matching WM_KEYDOWN/UP to the
// game window (no SendInput: nothing reaches the foreground application). Registered as a test aid.
std::string post_key(const std::string& keyboard, std::uint32_t vk, bool down);
#endif

void begin_capture();             // GUI key capture: the next key-down of any keyboard
std::uint32_t take_captured_key(); // 0 = none yet

} // namespace elmp::input::rawinput
