// ELMP manager (F1.17; v1 FR-UI-2a): bottom-right "ELMP" button; window with "Local Players" (players row
// with add / remove, PX storage state, devices × players matrix, skin / side / key bindings of the selected
// PX) and "Rules" (gameplay rules only, §7.13.1: map rule, PvP damage / knockback, equipAnywhere, recall,
// camera framing switch). Everything the user changes is a Request executed by party/manager on the game
// thread; this file keeps only view state (selected player, popups).

#include "overlay/draw.h"
#include "overlay/render.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace elmp::overlay {

namespace {

constexpr const char* kActionNames[kActions] = {"Up", "Down", "Left", "Right", "Jump", "Attack", "Skill 1", "Skill 2", "Dodge",
                                                "Hook", "Heal", "Switch set", "Interact", "Equipment menu", "HUD collapse"};

std::int64_t g_selected = 1;

void send(Req kind, std::int64_t slot = 0, double value = 0, const char* text = nullptr, bool keyboard = false, int action = 0) {
    Request r;
    r.kind = kind;
    r.slot = slot;
    r.value = value;
    r.keyboard = keyboard;
    r.action = action;
    if (text) r.text.set(text);
    request(r);
}

std::string label(std::int64_t slot) { return std::format("P{}", slot); }

void draw_button(float s) {
    const auto& io = ImGui::GetIO();
    const ImVec2 size(64 * s, 26 * s), pos(io.DisplaySize.x - size.x - 16 * s, io.DisplaySize.y - size.y - 16 * s);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##elmp_button", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
        const bool clicked = ImGui::InvisibleButton("##open", size), hovered = ImGui::IsItemHovered();
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 b(pos.x + size.x, pos.y + size.y);
        dl->AddRectFilled(pos, b, hovered ? IM_COL32(28, 24, 30, 225) : IM_COL32(12, 12, 16, 170), 4 * s);
        dl->AddRect(pos, b, hovered ? IM_COL32(240, 210, 130, 230) : IM_COL32(200, 195, 210, 110), 4 * s, 0, s);
        const float fs = 16 * s;
        const auto ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, "ELMP");
        dl->AddText(ImGui::GetFont(), fs, ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
                    hovered ? IM_COL32(250, 228, 160, 255) : IM_COL32(225, 200, 135, 235), "ELMP");
        if (clicked) send(Req::ManagerOpen);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// The game's cursor is a UMG software cursor (WBP_Cursor: T_MouseCursor, offset -10/-5, 32×32 UMG units)
// drawn inside the game image, so ELMP windows cover it: draw the same sprite again on top.
void draw_cursor() {
    const auto& io = ImGui::GetIO();
    if (!io.WantCaptureMouse || io.MousePos.x < 0) return;
    float w{}, h{};
    const auto tex = icon_texture("ui_mouse_cursor", w, h);
    if (!tex) return;
    const float umg = io.DisplaySize.y / 1080.0f;
    const ImVec2 a(io.MousePos.x - 10 * umg, io.MousePos.y - 5 * umg);
    ImGui::GetForegroundDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(tex)), a, ImVec2(a.x + 32 * umg, a.y + 32 * umg));
}

bool pill(const std::string& text, bool selected, bool down, float s) {
    const ImVec2 size(52 * s, 26 * s), a = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(text.c_str(), size), hovered = ImGui::IsItemHovered();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, selected ? IM_COL32(64, 54, 52, 250) : hovered ? IM_COL32(48, 42, 50, 240) : IM_COL32(30, 28, 36, 230), 4 * s);
    dl->AddRect(a, b, selected ? IM_COL32(240, 210, 130, 230) : IM_COL32(200, 195, 210, hovered ? 120 : 70), 4 * s, 0, s);
    const auto ts = ImGui::CalcTextSize(text.c_str());
    dl->AddText(ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f),
                down ? IM_COL32(230, 110, 110, 255) : selected ? IM_COL32(250, 228, 160, 255) : IM_COL32(235, 232, 240, 255), text.c_str());
    if (down && hovered) ImGui::SetTooltip("Down - revives near P1");
    return clicked;
}

void draw_players_row(const Manager& m, float s) {
    const float row_h = 26 * s, y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y + (row_h - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextUnformatted("Players");
    ImGui::SameLine(0, 12 * s);
    ImGui::SetCursorPosY(y);
    if (pill("P1", g_selected == 1, false, s)) g_selected = 1;
    for (int i = 0; i < m.nseats; ++i) {
        ImGui::SameLine(0, 6 * s);
        ImGui::SetCursorPosY(y);
        if (pill(label(m.seats[i].slot), g_selected == m.seats[i].slot, !m.seats[i].alive, s)) g_selected = m.seats[i].slot;
    }
    const auto& st = ImGui::GetStyle();
    const std::string remove = g_selected != 1 ? std::format("Remove {}", label(g_selected)) : std::string();
    const auto bw = [&](const std::string& t) { return ImGui::CalcTextSize(t.c_str()).x + st.FramePadding.x * 2; };
    float width = bw("Add player");
    if (!remove.empty()) width += st.ItemSpacing.x + bw(remove);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - width);
    ImGui::SetCursorPosY(y);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x, (row_h - ImGui::GetTextLineHeight()) * 0.5f));
    if (ImGui::Button("Add player")) send(Req::AddPlayer);
    if (!remove.empty()) {
        ImGui::SameLine();
        ImGui::SetCursorPosY(y);
        if (ImGui::Button(remove.c_str())) send(Req::RemovePlayer, g_selected);
    }
    ImGui::PopStyleVar();
}

void draw_storage(const Manager& m) {
    if (!m.storage_id.empty()) ImGui::TextDisabled("PX save: %s", m.storage_id.c_str());
    if (!m.storage_message.empty()) ImGui::TextWrapped("%s", m.storage_message.c_str());
    if (m.storage_blocked) {
        if (ImGui::Button("New PX data")) ImGui::OpenPopup("New PX data?");
        if (m.storage_can_reuse) {
            ImGui::SameLine();
            if (ImGui::Button("Reuse previous PX data")) ImGui::OpenPopup("Reuse PX data?");
        }
    }
    struct Choice {
        const char *popup, *text;
        Req req;
    };
    for (const auto& c : {Choice{"New PX data?", "Start this save with no PX party. Older PX data is kept untouched.", Req::StorageNew},
                          Choice{"Reuse PX data?", "Associate this save with the previous PX data. Equipment is checked against P1.", Req::StorageReuse}}) {
        if (ImGui::BeginPopupModal(c.popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", c.text);
            if (ImGui::Button("Confirm")) {
                send(c.req);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
}

void draw_devices(const Manager& m) {
    bool muted = m.muted;
    if (ImGui::Checkbox("Unbind all local inputs", &muted)) send(Req::Mute, 0, muted ? 1 : 0);
    ImGui::SameLine();
    ImGui::TextDisabled(muted ? "All keyboards & gamepads are muted for this game instance." : "Press a key to find a device. Unassigned devices control P1.");
    if (!ImGui::BeginTable("devices", m.nseats + 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("P1");
    for (int i = 0; i < m.nseats; ++i) ImGui::TableSetupColumn(label(m.seats[i].slot).c_str());
    ImGui::TableHeadersRow();
    for (int d = 0; d < m.ndevices; ++d) {
        const auto& dev = m.devices[d];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(dev.active ? ImVec4(0.95f, 0.8f, 0.4f, 1) : ImVec4(0.35f, 0.35f, 0.4f, 1), dev.active ? "(*)" : "( )");
        ImGui::SameLine();
        if (dev.connected) ImGui::TextUnformatted(dev.name.c_str());
        else ImGui::TextDisabled("%s (not connected)", dev.name.c_str());
        for (int col = 0; col <= m.nseats; ++col) {
            const std::int64_t p = col == 0 ? 1 : m.seats[col - 1].slot;
            ImGui::TableSetColumnIndex(col + 1);
            bool on = dev.assigned == p || (dev.assigned <= 1 && p == 1);
            ImGui::PushID(d * 64 + col);
            if (ImGui::Checkbox("##on", &on)) send(Req::Assign, on ? p : 1, 0, dev.id.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void draw_seat(const Manager& m, const ManagerSeat& seat, float s) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s skin", label(seat.slot).c_str());
    ImGui::SameLine(0, 10 * s);
    ImGui::SetNextItemWidth(160 * s);
    if (ImGui::BeginCombo("##skin", std::format("p{:04}_Lily", seat.skin).c_str())) {
        for (int n = 1; n <= 6; ++n)
            if (ImGui::Selectable(std::format("p{:04}_Lily", n).c_str(), n == seat.skin)) send(Req::Skin, seat.slot, n);
        ImGui::EndCombo();
    }
    static const char* sides[] = {"", "Friendly", "Enemy"};
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s side", label(seat.slot).c_str());
    ImGui::SameLine(0, 10 * s);
    ImGui::SetNextItemWidth(160 * s);
    if (ImGui::BeginCombo("##side", sides[std::clamp<int>(seat.team, 1, 2)])) {
        for (int n = 1; n <= 2; ++n)
            if (ImGui::Selectable(sides[n], n == seat.team)) send(Req::Team, seat.slot, n);
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 10 * s);
    ImGui::TextDisabled("enemy players can hurt friendly ones");
    ImGui::Separator();
    ImGui::Text("%s key bindings", label(seat.slot).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset to defaults")) send(Req::ResetBindings, seat.slot);
    if (m.capturing) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel capture")) send(Req::CancelCapture);
    }
    if (!ImGui::BeginTable("bindings", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) return;
    ImGui::TableSetupColumn("Action");
    ImGui::TableSetupColumn("Keyboard");
    ImGui::TableSetupColumn("Gamepad");
    ImGui::TableHeadersRow();
    for (int a = 0; a < kActions; ++a) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(kActionNames[a]);
        for (int col = 0; col < 2; ++col) {
            const bool keyboard = col == 0;
            ImGui::TableSetColumnIndex(col + 1);
            const bool capturing = m.capturing && m.capture_slot == seat.slot && m.capture_action == a && m.capture_keyboard == keyboard;
            std::string text = capturing ? (keyboard ? "Press a key..." : "Press a button...") : std::string((keyboard ? seat.keys[a] : seat.pads[a]).c_str());
            if (text.empty()) text = "-";
            ImGui::PushID(a * 2 + col);
            if (ImGui::Button(text.c_str(), ImVec2(-1, 0)) && !capturing) send(Req::Capture, seat.slot, 0, nullptr, keyboard, a);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

template <std::size_t N>
void combo(const char* id, const char* caption, const char* const (&names)[N], const double (&values)[N], double current, const char* key, const char* note, float s) {
    int sel = 0;
    for (std::size_t i = 0; i < N; ++i)
        if (std::abs(values[i] - current) < 1e-6) sel = static_cast<int>(i);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(caption);
    ImGui::SameLine(210 * s);
    ImGui::SetNextItemWidth(200 * s);
    if (ImGui::BeginCombo(id, names[sel])) {
        for (std::size_t i = 0; i < N; ++i)
            if (ImGui::Selectable(names[i], static_cast<int>(i) == sel)) send(Req::Rule, 0, values[i], key);
        ImGui::EndCombo();
    }
    if (note) {
        ImGui::SameLine(0, 10 * s);
        ImGui::TextDisabled("%s", note);
    }
}

void check(const char* caption, bool value, const char* key, const char* note, float s) {
    bool v = value;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(caption);
    ImGui::SameLine(210 * s);
    ImGui::PushID(key);
    if (ImGui::Checkbox("##v", &v)) send(Req::Rule, 0, v ? 1 : 0, key);
    ImGui::PopID();
    ImGui::SameLine(0, 10 * s);
    ImGui::TextDisabled("%s", note);
}

void number(const char* caption, int value, const char* key, int lo, int hi, const char* note, float s) {
    int v = value;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(caption);
    ImGui::SameLine(210 * s);
    ImGui::SetNextItemWidth(200 * s);
    ImGui::PushID(key);
    if (ImGui::InputInt("##v", &v, lo >= 1000 ? 100 : 50, 500, ImGuiInputTextFlags_EnterReturnsTrue)) send(Req::Rule, 0, std::clamp(v, lo, hi), key);
    ImGui::PopID();
    ImGui::SameLine(0, 10 * s);
    ImGui::TextDisabled("%s", note);
}

void draw_rules(const Rules& r, float s) {
    static const char* const maps[] = {"1 - anyone at a door", "2 - everyone at the door", "3 - only P1 (vanilla)"};
    static const double map_values[] = {1, 2, 3};
    combo("##mapRule", "Map change", maps, map_values, r.map_rule, "mapRule", nullptr, s);
    static const char* const dmg[] = {"5%", "10%", "25%", "50%", "100%"};
    static const double dmg_values[] = {0.05, 0.1, 0.25, 0.5, 1.0};
    combo("##pvpDamage", "PvP damage", dmg, dmg_values, r.pvp_damage_pct / 100.0, "pvpDamage", "monsters unaffected", s);
    static const char* const kb[] = {"Native", "Light", "Medium", "Heavy"};
    static const double kb_values[] = {0, 1, 2, 3};
    combo("##pvpKnockback", "PvP knockback", kb, kb_values, r.knockback, "pvpKnockback",
          r.knockback == 0 ? "game rule: launched once stamina breaks" : "knockback attacks launch enemy players every hit", s);
    ImGui::Separator();
    check("Equip anywhere", r.equip_anywhere, "equipAnywhere", "PX may change spirits / relics away from a bench", s);
    check("Recall", r.recall, "recall", "bring PX back to P1 when they fall out or stay too far", s);
    ImGui::BeginDisabled(!r.recall);
    number("Recall distance", r.recall_distance, "recallDistance", 300, 100000, "UU from P1", s);
    number("Recall delay", r.recall_delay_ms, "recallDelayMs", 0, 60000, "ms that far before recall", s);
    number("Recall fall depth", r.recall_fall_depth, "recallFallDepth", 300, 100000, "UU below P1: recalled at once", s);
    ImGui::EndDisabled();
    ImGui::Separator();
    check("Shared camera framing", r.camera_framing, "cameraFraming", "widen the camera when PX are present (margins: elmp set camera...)", s);
}

void draw_window(const Manager& m, double now) {
    const auto& io = ImGui::GetIO();
    const float s = io.DisplaySize.y / 720.0f;
    ImGui::SetNextWindowSize(ImVec2(780 * s, 560 * s), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("ELMP", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("pages")) {
            if (ImGui::BeginTabItem("Local Players")) {
                const ManagerSeat* selected = nullptr;
                for (int i = 0; i < m.nseats; ++i)
                    if (m.seats[i].slot == g_selected) selected = &m.seats[i];
                if (!selected) g_selected = 1;
                draw_storage(m);
                ImGui::BeginDisabled(m.storage_blocked);
                draw_players_row(m, s);
                ImGui::EndDisabled();
                ImGui::Separator();
                draw_devices(m);
                ImGui::Separator();
                if (selected) draw_seat(m, *selected, s);
                else ImGui::TextDisabled("P1 uses the game's own key settings. Select another player to edit their key bindings.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Online")) {
                static char code[32] = "", addr[64] = "127.0.0.1";
                static int port = 7777;
                ImGui::TextUnformatted(m.net_status.c_str());
                if (m.net_role == 2 && ImGui::Button("Equipment")) send(Req::GuestEquipment);
                if (!m.net_error.view().empty()) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "Last: %s", m.net_error.c_str());
                ImGui::Separator();
                ImGui::BeginDisabled(m.net_role != 0);
                ImGui::SetNextItemWidth(200 * s);
                ImGui::InputText("Pairing code", code, sizeof(code));
                ImGui::SetNextItemWidth(200 * s);
                ImGui::InputInt("UDP port", &port);
                port = std::clamp(port, 1, 65535);
                if (ImGui::Button("Host")) send(Req::NetHost, 0, 0, std::format("{}|{}|", code, port).c_str());
                ImGui::Separator();
                ImGui::SetNextItemWidth(200 * s);
                ImGui::InputText("Host address", addr, sizeof(addr));
                if (ImGui::Button("Join")) send(Req::NetJoin, 0, 0, std::format("{}|{}|{}", code, port, addr).c_str());
                ImGui::EndDisabled();
                ImGui::BeginDisabled(m.net_role == 0);
                if (ImGui::Button("Leave")) send(Req::NetLeave);
                ImGui::EndDisabled();
                ImGui::TextDisabled("Both players need the same game, ELMP build and data; the joiner sees the reason if not.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Rules")) {
                draw_rules(m.rules, s);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!m.message.empty() && now < m.message_until) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.45f, 1), "%s", m.message.c_str());
    }
    ImGui::End();
    if (!open) send(Req::ManagerClose);
}

} // namespace

void draw_manager(const Manager& m, double now) {
    if (m.open) draw_window(m, now);
    else if (m.button) draw_button(ImGui::GetIO().DisplaySize.y / 720.0f);
    draw_cursor();
}

} // namespace elmp::overlay
