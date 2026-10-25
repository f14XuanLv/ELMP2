// HUD for local seats (F1.7; v1 party-facts「HUD 数据」): head marker (label, HP bar, prayer dots, prompt,
// toast) and a right-edge panel per seat (HP, prayers, special gauge like WBP_MPGauge, three skill slots
// like the vanilla bottom-left bar: round icon, key badge, remaining uses, cooldown sweep). Collapsed = a
// narrow strip with a vertical HP bar. Scaled from 720p.

#include "overlay/draw.h"
#include "overlay/render.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>

namespace elmp::overlay {

namespace {

constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

const ImU32 panel_bg = rgba(12, 12, 16, 170), panel_edge = rgba(200, 200, 210, 90);
const ImU32 text_main = rgba(235, 235, 240), text_enemy = rgba(190, 120, 240); // enemy seats: violet (not HP red / MP blue / gold)
const ImU32 hp_fill = rgba(196, 52, 60), hp_back = rgba(60, 20, 24, 200);
const ImU32 mp_fill = rgba(90, 140, 230), mp_ready = rgba(240, 210, 110), bar_back = rgba(30, 30, 40, 200);
const ImU32 heal_on = rgba(245, 235, 200), heal_off = rgba(80, 80, 90);

struct Painter {
    ImDrawList* dl;
    ImFont* font;
    float s;
    float px(float v) const { return v * s; }
    ImVec2 size(const char* t, float sz) const { return font->CalcTextSizeA(px(sz), FLT_MAX, 0.0f, t); }
    void text(ImVec2 p, const char* t, float sz, ImU32 col) const {
        dl->AddText(font, px(sz), ImVec2(p.x + 1, p.y + 1), rgba(0, 0, 0, 200), t);
        dl->AddText(font, px(sz), p, col, t);
    }
    void centered(ImVec2 c, const char* t, float sz, ImU32 col) const { text(ImVec2(c.x - size(t, sz).x * 0.5f, c.y), t, sz, col); }
    void bar(ImVec2 p, float w, float h, float r, ImU32 fill, ImU32 back) const {
        r = std::clamp(r, 0.0f, 1.0f);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), back, px(1));
        if (r > 0) dl->AddRectFilled(p, ImVec2(p.x + w * r, p.y + h), fill, px(1));
    }
    void heals(ImVec2 p, int n, int max, float r) const {
        for (int i = 0; i < max; ++i) dl->AddCircleFilled(ImVec2(p.x + i * r * 2.6f + r, p.y + r), r, i < n ? heal_on : heal_off);
    }
};

float ratio(float v, float max) { return max > 0 ? v / max : 0.0f; }
ImU32 label_color(const HudSeat& s) { return s.team == 2 ? text_enemy : text_main; }
std::string label(const HudSeat& s) { return std::format("P{}", s.slot); }

std::string count(const HudSkill& k) {
    if (k.limit < 0) return "?";
    if (k.limit == 0) return "\xE2\x88\x9E"; // ∞
    return k.remaining >= 0 ? std::to_string(k.remaining) : "?";
}
bool exhausted(const HudSkill& k) { return k.limit > 0 && k.remaining == 0; }

// Anchor rounded to whole pixels: sub-pixel camera motion would make the glyphs shimmer.
void draw_follow(const Painter& pt, const HudSeat& s, double now) {
    if (!s.anchored) return;
    const ImVec2 c(std::round(s.ax), std::round(s.ay));
    const float w = pt.px(54), h = pt.px(5);
    pt.centered(ImVec2(c.x, c.y - pt.px(30)), label(s).c_str(), 15, label_color(s));
    pt.bar(ImVec2(c.x - w * 0.5f, c.y - pt.px(12)), w, h, ratio(s.hp, s.max_hp), hp_fill, hp_back);
    const float r = pt.px(2.4f), dots = s.max_heals * r * 2.6f;
    pt.heals(ImVec2(c.x - dots * 0.5f, c.y - pt.px(5)), s.heals, s.max_heals, r);
    if (!s.toast.empty() && now < s.toast_until) pt.centered(ImVec2(c.x, c.y - pt.px(52)), s.toast.c_str(), 15, mp_ready);
    if (!s.prompt.empty()) pt.centered(ImVec2(c.x, c.y + pt.px(6)), s.prompt.c_str(), 15, text_main);
}

// WBP_MPGauge: segment 1 = MP / min(maxMP, cost); a second segment when maxMP > cost; dark until special unlocked.
void draw_special(const Painter& pt, const HudSeat& s, ImVec2 p, float width, float height) {
    const float cost = s.special_cost;
    const bool two = s.max_mp > cost;
    const float gap = pt.px(4), seg = two ? (width - gap) * 0.5f : width;
    const float first = ratio(s.mp, std::min(s.max_mp, cost)), second = two ? ratio(s.mp - cost, s.max_mp - cost) : 0.0f;
    const auto col = [&](float r) { return !s.special_unlocked ? heal_off : (r >= 1.0f ? mp_ready : mp_fill); };
    pt.bar(p, seg, height, s.special_unlocked ? first : 0.0f, col(first), bar_back);
    if (two) pt.bar(ImVec2(p.x + seg + gap, p.y), seg, height, s.special_unlocked ? second : 0.0f, col(second), bar_back);
}

void draw_skills(const Painter& pt, const HudSeat& s, ImVec2 p, float width) {
    const float size = pt.px(28), gap = (width - size * 3) / 2.0f;
    for (int i = 0; i < 3; ++i) {
        const auto& k = s.skills[i];
        const ImVec2 c(p.x + i * (size + gap) + size * 0.5f, p.y + size * 0.5f);
        pt.dl->AddCircleFilled(c, size * 0.5f, rgba(20, 18, 24, 220));
        pt.dl->AddCircle(c, size * 0.5f, panel_edge, 0, pt.px(1));
        float iw{}, ih{};
        if (const auto tex = k.icon.empty() ? 0ull : icon_texture(k.icon.c_str(), iw, ih); tex && iw > 0 && ih > 0) {
            const float box = size * 0.82f, f = box / std::max(iw, ih);
            const ImVec2 a(c.x - iw * f * 0.5f, c.y - ih * f * 0.5f);
            const bool dim = exhausted(k) || k.limit < 0;
            pt.dl->AddImage(ImTextureRef(static_cast<ImTextureID>(tex)), a, ImVec2(a.x + iw * f, a.y + ih * f), ImVec2(0, 0), ImVec2(1, 1),
                            dim ? rgba(110, 110, 120) : rgba(255, 255, 255));
        }
        if (!k.icon.empty() && k.cooldown > 0) {
            const float radius = size * 0.48f, start = -std::numbers::pi_v<float> * 0.5f, arc = std::numbers::pi_v<float> * 2 * k.cooldown;
            const int segments = std::max(1, static_cast<int>(std::ceil(32 * k.cooldown)));
            const auto at = [&](float a) { return ImVec2(c.x + radius * std::cos(a), c.y + radius * std::sin(a)); };
            for (int n = 0; n < segments; ++n) pt.dl->AddTriangleFilled(c, at(start + arc * n / segments), at(start + arc * (n + 1) / segments), rgba(8, 8, 14, 175));
            pt.dl->PathArcTo(c, radius, start, start + arc, segments);
            pt.dl->PathStroke(mp_ready, 0, pt.px(1.4f));
        }
        if (!k.key.empty()) {
            const ImVec2 b(c.x - size * 0.5f - pt.px(2), c.y - size * 0.5f - pt.px(2));
            const auto ks = pt.size(k.key.c_str(), 10);
            pt.dl->AddRectFilled(b, ImVec2(b.x + ks.x + pt.px(4), b.y + ks.y + pt.px(1)), rgba(60, 60, 70, 230), pt.px(2));
            pt.text(ImVec2(b.x + pt.px(2), b.y), k.key.c_str(), 10, text_main);
        }
        if (!k.icon.empty()) pt.centered(ImVec2(c.x, c.y + size * 0.5f - pt.px(2)), count(k).c_str(), 12, exhausted(k) ? hp_fill : text_main);
    }
}

float draw_panel(const Painter& pt, const HudSeat& s, float right, float top) {
    const auto name = label(s);
    if (s.collapsed) {
        const float w = pt.px(30), h = pt.px(58);
        const ImVec2 a(right - w, top);
        pt.dl->AddRectFilled(a, ImVec2(right, top + h), panel_bg, pt.px(4), ImDrawFlags_RoundCornersLeft);
        pt.centered(ImVec2(a.x + w * 0.5f, a.y + pt.px(4)), name.c_str(), 14, label_color(s));
        const float bh = pt.px(30), bw = pt.px(6), filled = bh * std::clamp(ratio(s.hp, s.max_hp), 0.0f, 1.0f);
        const ImVec2 b(a.x + (w - bw) * 0.5f, a.y + pt.px(24));
        pt.dl->AddRectFilled(b, ImVec2(b.x + bw, b.y + bh), hp_back);
        pt.dl->AddRectFilled(ImVec2(b.x, b.y + bh - filled), ImVec2(b.x + bw, b.y + bh), hp_fill);
        return h;
    }
    const float w = pt.px(132), pad = pt.px(7), h = pt.px(104);
    const ImVec2 a(right - w, top), b(right, top + h);
    pt.dl->AddRectFilled(a, b, panel_bg, pt.px(5), ImDrawFlags_RoundCornersLeft);
    pt.dl->AddRect(a, b, panel_edge, pt.px(5), ImDrawFlags_RoundCornersLeft);
    float y = a.y + pad;
    const float x = a.x + pad, inner = w - pad * 2;
    pt.text(ImVec2(x, y - pt.px(1)), name.c_str(), 15, label_color(s));
    for (int i = 0; i < 2; ++i) pt.dl->AddCircleFilled(ImVec2(x + inner - pt.px(14) + i * pt.px(9), y + pt.px(8)), pt.px(2.6f), s.summon_set == i + 1 ? text_main : heal_off);
    y += pt.px(19);
    pt.bar(ImVec2(x, y), inner, pt.px(5), ratio(s.hp, s.max_hp), hp_fill, hp_back);
    y += pt.px(8);
    pt.heals(ImVec2(x, y), s.heals, s.max_heals, pt.px(2.4f));
    y += pt.px(8);
    draw_special(pt, s, ImVec2(x, y), inner, pt.px(3));
    y += pt.px(10);
    draw_skills(pt, s, ImVec2(x, y), inner);
    return h;
}

} // namespace

void draw_hud(const Hud& m, ImFont* font, double now) {
    if (!m.visible || m.count <= 0) return;
    const auto& io = ImGui::GetIO();
    const Painter pt{ImGui::GetForegroundDrawList(), font, io.DisplaySize.y / 720.0f};
    const int n = std::min(m.count, kMaxSeats);
    for (int i = 0; i < n; ++i) draw_follow(pt, m.seats[i], now);
    float top = pt.px(110);
    for (int i = 0; i < n; ++i) top += draw_panel(pt, m.seats[i], io.DisplaySize.x, top) + pt.px(8);
}

} // namespace elmp::overlay
