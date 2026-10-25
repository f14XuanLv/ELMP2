#pragma once

// Render-thread drawing of the published models (ImGui). Drawing never changes game state; ELMP window
// actions are queued as Requests (overlay/model.h).

#include "overlay/model.h"

struct ImFont;

namespace elmp::overlay {

void draw_hud(const Hud& m, ImFont* font, double now);
void draw_menu(const Menu& m, ImFont* font, double now);
void draw_manager(const Manager& m, double now);

} // namespace elmp::overlay
