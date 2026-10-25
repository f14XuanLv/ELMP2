#pragma once

// `elmp …` console commands (§7.8). Installed as a UE4SS ProcessConsoleExec post-callback, which UE4SS
// only invokes for UGameViewportClient — the local player's console path (Controller0); controllers of
// other seats have no LocalPlayer and never reach it. Output goes to the in-game console, UE4SS.log and,
// in Dev builds, back to the caller of ELMP_Command (UEBridge).

#include <span>
#include <string>
#include <string_view>

namespace elmp::console {

using Args = std::span<const std::string>;
using Handler = void (*)(Args args, std::string& out);

void add(const char* name, const char* usage, Handler handler);
// `line` is the full command ("elmp status ..."). Returns false if it is not an elmp command.
bool run(std::string_view line, std::string& out);

void install();
void uninstall();

void register_p0_commands(); // commands.cpp

} // namespace elmp::console
