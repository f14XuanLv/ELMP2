#pragma once

// XInput wiring (M6): the game's import of XINPUT1_3.dll ordinal 2 (XInputGetState) is redirected
// (import table entry, no code bytes, no RVA). The entry must still point at the system DLL's export,
// otherwise another tool changed it and we refuse. While the router is inactive the hook calls straight
// through (G0). Active:
//   - pad 0 → merge of every connected pad owned by P1; none → neutral if pad 0 is connected, else as is;
//   - pads 1..3 → "connected, no input" when connected (owned by a seat or merged into 0).

#include "input/bindings.h"

#include <optional>
#include <string>

namespace elmp::input::xinput {

std::string install(); // empty = ok (idempotent)
void uninstall();
bool installed();

std::optional<gamepad::State> read(int index); // real state through the system function
void refresh_connected();                      // probes 0..3 (low frequency; disconnected probes are slow)

#ifdef ELMP_DEV
// Dev test seam: a simulated pad on index N seen by read() (routing, seat input, P1 merge); the game's own
// probe of an unplugged index still reports "not connected". Registered as a test aid when used.
void set_fake(int index, const std::optional<gamepad::State>& state);
#endif

} // namespace elmp::input::xinput
