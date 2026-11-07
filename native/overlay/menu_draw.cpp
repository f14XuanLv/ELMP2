// PX equipment menu (F1.10; v1 party-facts「原版装备菜单」): tabs Spirits / Relics, set A / B with three
// cells each, item grid, description panel, key hints. Navigation state comes from party/menu_ctrl.

#include "overlay/draw.h"
#include "overlay/render.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>

namespace elmp::overlay {

namespace {

constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

const ImU32 dim_bg = rgba(0, 0, 0, 150), win_bg = rgba(14, 13, 18, 235), edge = rgba(200, 195, 210, 110);
const ImU32 text_main = rgba(235, 232, 240), text_dim = rgba(150, 148, 160), accent = rgba(240, 210, 130);
const ImU32 cell_bg = rgba(30, 28, 36, 230), cursor_col = rgba(240, 210, 130, 255), warn = rgba(230, 110, 110);

struct P {
    ImDrawList* dl;
    ImFont* font;
    float s;
    float px(float v) const { return v * s; }
    ImVec2 size(const char* t, float sz, float wrap = 0) const { return font->CalcTextSizeA(px(sz), FLT_MAX, wrap, t); }
    void text(ImVec2 p, const char* t, float sz, ImU32 c, float wrap = 0) const { dl->AddText(font, px(sz), p, c, t, nullptr, wrap); }
    void icon(const char* id, ImVec2 center, float box) const {
        float w{}, h{};
        const auto tex = icon_texture(id, w, h);
        if (!tex || w <= 0 || h <= 0) return;
        const float k = box / std::max(w, h);
        const ImVec2 a(center.x - w * k * 0.5f, center.y - h * k * 0.5f);
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(tex)), a, ImVec2(a.x + w * k, a.y + h * k));
    }
};

const MenuItem* find_spirit(const Menu& m, const char* id) {
    for (int i = 0; i < m.nspirits; ++i)
        if (m.spirits[i].id.view() == id) return &m.spirits[i];
    return nullptr;
}

void draw_tabs(const P& p, const Menu& m, ImVec2 a, float w) {
    const std::array<const char*, 2> names{"Spirits", "Relics"};
    float x = a.x + p.px(16);
    p.text(ImVec2(x, a.y + p.px(12)), m.prev.c_str(), 12, text_dim);
    x += std::max(p.px(28), p.size(m.prev.c_str(), 12).x + p.px(10));
    for (int i = 0; i < 2; ++i) {
        const bool on = m.page == i;
        p.text(ImVec2(x, a.y + p.px(8)), names[i], 20, on ? accent : text_dim);
        if (on) p.dl->AddLine(ImVec2(x, a.y + p.px(34)), ImVec2(x + p.size(names[i], 20).x, a.y + p.px(34)), accent, p.px(2));
        x += p.size(names[i], 20).x + p.px(24);
    }
    p.text(ImVec2(x - p.px(8), a.y + p.px(12)), m.next.c_str(), 12, text_dim);
    const auto seat = m.online ? std::string("You") : std::format("P{}", m.slot);
    p.text(ImVec2(a.x + w - p.px(16) - p.size(seat.c_str(), 18).x, a.y + p.px(10)), seat.c_str(), 18, text_main);
}

void draw_description(const P& p, const MenuItem* item, ImVec2 a, float w, float h, const std::string& extra) {
    p.dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), cell_bg, p.px(4));
    if (!item) return;
    p.icon(item->icon.c_str(), ImVec2(a.x + p.px(30), a.y + p.px(30)), p.px(44));
    p.text(ImVec2(a.x + p.px(60), a.y + p.px(10)), item->name.c_str(), 17, text_main, w - p.px(70));
    if (!extra.empty()) p.text(ImVec2(a.x + p.px(60), a.y + p.px(32)), extra.c_str(), 13, accent);
    p.text(ImVec2(a.x + p.px(12), a.y + p.px(62)), item->desc.c_str(), 14, text_dim, w - p.px(24));
}

void draw_grid(const P& p, const Menu& m, ImVec2 a, bool spirits) {
    const auto* list = spirits ? m.spirits : m.passives;
    const int n = spirits ? m.nspirits : m.npassives;
    const float cell = p.px(52), gap = p.px(8);
    for (int i = 0; i < n; ++i) {
        const auto& item = list[i];
        const ImVec2 c0(a.x + (i % kMenuColumns) * (cell + gap), a.y + (i / kMenuColumns) * (cell + gap)), c1(c0.x + cell, c0.y + cell);
        p.dl->AddRectFilled(c0, c1, cell_bg, p.px(4));
        p.icon(item.icon.c_str(), ImVec2(c0.x + cell * 0.5f, c0.y + cell * 0.5f), cell * 0.78f);
        if (spirits) {
            p.text(ImVec2(c0.x + p.px(3), c1.y - p.px(15)), std::format("Lv{}", item.level).c_str(), 11, text_main);
        } else {
            for (int k = 0; k < item.slots; ++k) p.dl->AddCircleFilled(ImVec2(c0.x + p.px(6) + k * p.px(7), c1.y - p.px(6)), p.px(2.4f), accent);
            if (item.equipped) p.text(ImVec2(c1.x - p.px(14), c0.y + p.px(2)), "E", 12, accent);
        }
        if (i == m.cursor) p.dl->AddRect(ImVec2(c0.x - p.px(2), c0.y - p.px(2)), ImVec2(c1.x + p.px(2), c1.y + p.px(2)), cursor_col, p.px(5), 0, p.px(2));
    }
    if (n == 0) p.text(a, spirits ? "No spirits acquired" : "No relics acquired", 14, text_dim);
}

void draw_spirit_page(const P& p, const Menu& m, ImVec2 a, float w, float h) {
    const float cell = p.px(54), gap = p.px(12);
    for (int set = 0; set < 2; ++set) {
        const float y = a.y + set * (cell + p.px(34));
        p.text(ImVec2(a.x, y), set == 0 ? "Set A" : "Set B", 14, text_dim);
        const bool locked = set == 1 && m.set2_locked;
        for (int c = 0; c < 3; ++c) {
            const ImVec2 c0(a.x + c * (cell + gap), y + p.px(20)), center(c0.x + cell * 0.5f, c0.y + cell * 0.5f);
            p.dl->AddCircleFilled(center, cell * 0.5f, cell_bg);
            p.dl->AddCircle(center, cell * 0.5f, edge, 0, p.px(1));
            if (!locked && !m.sets[set][c].empty())
                if (const auto* sp = find_spirit(m, m.sets[set][c].c_str())) p.icon(sp->icon.c_str(), center, cell * 0.8f);
            p.text(ImVec2(c0.x - p.px(2), c0.y - p.px(2)), m.cells[c].c_str(), 12, text_main);
            if (m.set == set && m.cell == c) p.dl->AddCircle(center, cell * 0.5f + p.px(3), m.in_list ? text_dim : cursor_col, 0, p.px(2));
        }
        if (locked) p.text(ImVec2(a.x, y + p.px(36)), "Locked", 14, text_dim);
    }
    const float left_w = 3 * cell + 2 * gap + p.px(28), grid_w = kMenuColumns * p.px(60);
    const ImVec2 grid(a.x + left_w, a.y + p.px(20));
    if (m.in_list) draw_grid(p, m, grid, true);
    else p.text(grid, std::format("Select a slot with {} to choose a spirit", m.confirm.c_str()).c_str(), 14, text_dim, grid_w);
    const MenuItem* item = m.in_list ? (m.cursor < m.nspirits ? &m.spirits[m.cursor] : nullptr) : find_spirit(m, m.sets[m.set][m.cell].c_str());
    const float desc_x = grid.x + grid_w + p.px(12);
    draw_description(p, item, ImVec2(desc_x, a.y), a.x + w - desc_x, h, item ? std::format("Lv{}  {}", item->level, item->main ? "Main" : "Sub") : "");
}

void draw_passive_page(const P& p, const Menu& m, ImVec2 a, float w, float h) {
    const auto slots = std::format("Slots : {} / {}", m.used_slots, m.max_slots);
    p.text(a, slots.c_str(), 16, m.used_slots > m.max_slots ? warn : text_main);
    const float bar_w = p.px(300), bar_y = a.y + p.px(26), step = bar_w / std::max(1, m.max_slots);
    for (int i = 0; i < m.max_slots; ++i)
        p.dl->AddRectFilled(ImVec2(a.x + i * step, bar_y), ImVec2(a.x + (i + 1) * step - p.px(2), bar_y + p.px(6)), i < m.used_slots ? accent : cell_bg, p.px(1));
    draw_grid(p, m, ImVec2(a.x, a.y + p.px(46)), false);
    const MenuItem* item = m.cursor < m.npassives ? &m.passives[m.cursor] : nullptr;
    const float desc_x = a.x + kMenuColumns * p.px(60) + p.px(12);
    draw_description(p, item, ImVec2(desc_x, a.y), a.x + w - desc_x, h, item ? std::format("Slots {}{}", item->slots, item->equipped ? "   Equipped" : "") : "");
}

void draw_footer(const P& p, const Menu& m, ImVec2 a, float w, double now) {
    std::string hint = m.page == 0 ? (m.in_list ? std::format("{} Equip    {} Cancel", m.confirm.c_str(), m.back.c_str())
                                                : std::format("{} Select    {} Unequip    {} Close", m.confirm.c_str(), m.context.c_str(), m.back.c_str()))
                                   : std::format("{} Equip / Unequip    {} Close", m.confirm.c_str(), m.back.c_str());
    if (!m.can_change) hint += std::format("    ({})", m.reason.c_str());
    if (m.online) hint += "    World keeps running online";
    p.text(ImVec2(a.x + p.px(16), a.y + p.px(8)), hint.c_str(), 14, m.can_change ? text_dim : warn);
    if (!m.message.empty() && now < m.message_until) {
        const auto sz = p.size(m.message.c_str(), 15);
        p.text(ImVec2(a.x + w - sz.x - p.px(16), a.y + p.px(7)), m.message.c_str(), 15, warn);
    }
}

} // namespace

void draw_menu(const Menu& m, ImFont* font, double now) {
    if (!m.open) return;
    const auto& io = ImGui::GetIO();
    const P p{ImGui::GetForegroundDrawList(), font, io.DisplaySize.y / 720.0f};
    p.dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, dim_bg);
    const float w = p.px(1000), h = p.px(470);
    const ImVec2 a((io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f);
    p.dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), win_bg, p.px(8));
    p.dl->AddRect(a, ImVec2(a.x + w, a.y + h), edge, p.px(8));
    draw_tabs(p, m, a, w);
    const ImVec2 body(a.x + p.px(20), a.y + p.px(52));
    const float body_w = w - p.px(40), body_h = h - p.px(52) - p.px(44);
    if (m.page == 0) draw_spirit_page(p, m, body, body_w, body_h);
    else draw_passive_page(p, m, body, body_w, body_h);
    draw_footer(p, m, ImVec2(a.x, a.y + h - p.px(36)), w, now);
}

} // namespace elmp::overlay
