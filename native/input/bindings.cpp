#include "input/bindings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace elmp::input::bindings {

namespace {

constexpr std::array<std::string_view, Count> kNames{"up", "down", "left", "right", "jump", "attack", "summon1", "summon2", "dodge",
                                                     "hook", "heal", "summon_switch", "interact", "menu", "hud_toggle"};

constexpr std::uint32_t kTab = 0x09, kEnter = 0x0D, kEsc = 0x1B, kLeft = 0x25, kUp = 0x26, kRight = 0x27, kDown = 0x28;
constexpr std::uint32_t vk(char c) { return static_cast<std::uint32_t>(c); }

// XINPUT_GAMEPAD_*
constexpr std::uint32_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
                        kL3 = 0x0040, kR3 = 0x0080, kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

// ECommandInputTypes
enum Command : std::uint32_t { CJump = 1, CDodge = 2, CAttack = 3, CSummon1 = 4, CSummon2 = 5, CSwitch = 6, CHook = 7, CHeal = 8, CInteract = 9 };

float axis(std::int16_t v) { return std::clamp(static_cast<float>(v) / 32767.0f, -1.0f, 1.0f); }

bool ieq(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

Bindings make_defaults() {
    Bindings b;
    b.keys[Up] = {kUp};
    b.keys[Down] = {kDown};
    b.keys[Left] = {kLeft};
    b.keys[Right] = {kRight};
    b.keys[Jump] = {vk('Z')};
    b.keys[Attack] = {vk('X')};
    b.keys[Summon1] = {vk('S')};
    b.keys[Summon2] = {vk('D')};
    b.keys[Dodge] = {vk('C')};
    b.keys[Hook] = {vk('V')};
    b.keys[Heal] = {vk('A')};
    b.keys[SummonSwitch] = {vk('F')};
    b.keys[Interact] = {vk('V')};
    b.keys[Menu] = {kTab};
    b.keys[HudToggle] = {vk('Q')};
    b.pad[Up] = {kDpadUp};
    b.pad[Down] = {kDpadDown};
    b.pad[Left] = {kDpadLeft};
    b.pad[Right] = {kDpadRight};
    b.pad[Jump] = {kA};
    b.pad[Attack] = {kX};
    b.pad[Summon1] = {kY};
    b.pad[Summon2] = {kB};
    b.pad[Dodge] = {kRightTrigger};
    b.pad[Hook] = {kLeftTrigger};
    b.pad[Heal] = {kLB};
    b.pad[SummonSwitch] = {kRB};
    b.pad[Interact] = {kLeftTrigger};
    b.pad[Menu] = {kBack};
    b.pad[HudToggle] = {kR3};
    return b;
}

// Fixed menu keys: Jump = confirm, Summon2 / Menu = back, Attack = unequip, Heal / SummonSwitch = page,
// directions = move. Pad as vanilla; keyboard adds Enter (confirm), Esc / Tab (back), A / F (page).
Bindings make_menu_keys() {
    Bindings b = make_defaults();
    b.keys[Jump] = {vk('Z'), kEnter};
    b.keys[Summon2] = {kEsc, vk('D')};
    return b;
}

struct Label {
    std::uint32_t code;
    const char* name;
};
constexpr std::array<Label, 16> kPadLabels{{{kDpadUp, "DPad Up"}, {kDpadDown, "DPad Down"}, {kDpadLeft, "DPad Left"}, {kDpadRight, "DPad Right"},
                                            {kStart, "Start"}, {kBack, "Back"}, {kL3, "L3"}, {kR3, "R3"}, {kLB, "LB"}, {kRB, "RB"}, {kA, "A"}, {kB, "B"},
                                            {kX, "X"}, {kY, "Y"}, {kLeftTrigger, "LT"}, {kRightTrigger, "RT"}}};
constexpr std::array<Label, 31> kKeyLabels{{{0x08, "Backspace"}, {kTab, "Tab"}, {kEnter, "Enter"}, {0x10, "Shift"}, {0x11, "Ctrl"}, {0x12, "Alt"},
                                            {0x14, "CapsLock"}, {kEsc, "Esc"}, {0x20, "Space"}, {0x21, "PageUp"}, {0x22, "PageDown"}, {0x23, "End"},
                                            {0x24, "Home"}, {kLeft, "Left"}, {kUp, "Up"}, {kRight, "Right"}, {kDown, "Down"}, {0x2D, "Insert"},
                                            {0x2E, "Delete"}, {0xBA, ";"}, {0xBB, "="}, {0xBC, ","}, {0xBD, "-"}, {0xBE, "."}, {0xBF, "/"},
                                            {0xC0, "`"}, {0xDB, "["}, {0xDC, "\\"}, {0xDD, "]"}, {0xDE, "'"}, {0xA0, "LShift"}}};

} // namespace

std::string_view action_name(int a) { return a >= 0 && a < Count ? kNames[a] : std::string_view("?"); }

std::optional<int> action_named(std::string_view name) {
    for (int i = 0; i < Count; ++i)
        if (ieq(kNames[i], name)) return i;
    return std::nullopt;
}

const Bindings& defaults() {
    static const Bindings b = make_defaults();
    return b;
}

const Bindings& menu_keys() {
    static const Bindings b = make_menu_keys();
    return b;
}

json::Value to_json(const Bindings& b) {
    auto keys = json::Value::object(), pad = json::Value::object();
    for (int a = 0; a < Count; ++a) {
        auto k = json::Value::array(), p = json::Value::array();
        for (auto v : b.keys[a]) k.push(key_label(v));
        for (auto v : b.pad[a]) p.push(pad_label(v));
        keys.set(kNames[a], std::move(k));
        pad.set(kNames[a], std::move(p));
    }
    auto o = json::Value::object();
    o.set("keys", std::move(keys));
    o.set("pad", std::move(pad));
    return o;
}

Bindings from_json(const json::Value& v, const Bindings& base) {
    Bindings b = base;
    const auto table = [&](const char* name, auto& dst, auto parse) {
        const auto* t = v.get(name);
        if (!t || !t->is_object()) return;
        for (const auto& [action, list] : t->obj) {
            const auto a = action_named(action);
            if (!a || !list.is_array()) continue;
            std::vector<std::uint32_t> codes;
            for (const auto& e : list.arr)
                if (e.is_string())
                    if (auto c = parse(e.str)) codes.push_back(*c);
            dst[*a] = std::move(codes);
        }
    };
    table("keys", b.keys, key_named);
    table("pad", b.pad, pad_named);
    return b;
}

Actions from_keyboard(const Bindings& b, const std::bitset<256>& keys) {
    Actions out;
    for (int a = 0; a < Count; ++a)
        for (auto k : b.keys[a])
            if (k < 256 && keys.test(k)) out.held |= 1u << a;
    out.x = static_cast<float>((out.has(Right) ? 1 : 0) - (out.has(Left) ? 1 : 0));
    out.y = static_cast<float>((out.has(Up) ? 1 : 0) - (out.has(Down) ? 1 : 0));
    return out;
}

Actions from_pad(const Bindings& b, const gamepad::State& s) {
    Actions out;
    std::uint32_t controls = s.buttons;
    if (s.left_trigger > gamepad::kTriggerThreshold) controls |= kLeftTrigger;
    if (s.right_trigger > gamepad::kTriggerThreshold) controls |= kRightTrigger;
    for (int a = 0; a < Count; ++a)
        for (auto c : b.pad[a])
            if (controls & c) out.held |= 1u << a;
    // Left stick is always direction (analog); bound direction buttons (d-pad by default) win.
    const float lx = axis(s.thumb_lx), ly = axis(s.thumb_ly);
    if (std::hypot(lx, ly) > static_cast<float>(gamepad::kLeftThumbDeadzone) / 32767.0f) {
        out.x = lx;
        out.y = ly;
    }
    const int dx = (out.has(Right) ? 1 : 0) - (out.has(Left) ? 1 : 0);
    const int dy = (out.has(Up) ? 1 : 0) - (out.has(Down) ? 1 : 0);
    if (dx || dy) {
        out.x = static_cast<float>(dx);
        out.y = static_cast<float>(dy);
    }
    return out;
}

Actions merge(const Actions& a, const Actions& b) {
    Actions out;
    out.held = a.held | b.held;
    out.x = std::abs(b.x) > std::abs(a.x) ? b.x : a.x;
    out.y = std::abs(b.y) > std::abs(a.y) ? b.y : a.y;
    return out;
}

std::uint32_t command_bits(const Actions& a) {
    std::uint32_t raw = 0;
    const auto map = [&](int action, Command c) {
        if (a.has(action)) raw |= 1u << c;
    };
    map(Jump, CJump);
    map(Attack, CAttack);
    map(Summon1, CSummon1);
    map(Summon2, CSummon2);
    map(Dodge, CDodge);
    map(Hook, CHook);
    map(Heal, CHeal);
    map(SummonSwitch, CSwitch);
    map(Interact, CInteract);
    return raw;
}

std::string key_label(std::uint32_t k) {
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) return std::string(1, static_cast<char>(k));
    for (const auto& l : kKeyLabels)
        if (l.code == k) return l.name;
    if (k >= 0x60 && k <= 0x69) return std::format("Num{}", k - 0x60);
    if (k >= 0x70 && k <= 0x87) return std::format("F{}", k - 0x6F);
    return std::format("VK{}", k);
}

std::string pad_label(std::uint32_t c) {
    for (const auto& l : kPadLabels)
        if (l.code == c) return l.name;
    return std::format("Pad{}", c);
}

std::optional<std::uint32_t> key_named(std::string_view s) {
    if (s.size() == 1 && std::isalnum(static_cast<unsigned char>(s[0]))) return static_cast<std::uint32_t>(std::toupper(static_cast<unsigned char>(s[0])));
    for (const auto& l : kKeyLabels)
        if (ieq(l.name, s)) return l.code;
    std::uint32_t v = 0;
    const auto num = [&](std::string_view t, int base) { return std::from_chars(t.data(), t.data() + t.size(), v, base).ec == std::errc{} && v > 0 && v < 256; };
    if (s.size() > 3 && ieq(s.substr(0, 3), "Num") && num(s.substr(3), 10) && v <= 9) return 0x60 + v;
    if (s.size() > 1 && (s[0] == 'F' || s[0] == 'f') && num(s.substr(1), 10) && v >= 1 && v <= 24) return 0x6F + v;
    if (s.size() > 2 && ieq(s.substr(0, 2), "VK") && num(s.substr(2), 10)) return v;
    if (s.size() > 2 && ieq(s.substr(0, 2), "0x") && num(s.substr(2), 16)) return v;
    return std::nullopt;
}

std::optional<std::uint32_t> pad_named(std::string_view s) {
    for (const auto& l : kPadLabels)
        if (ieq(l.name, s)) return l.code;
    return std::nullopt;
}

} // namespace elmp::input::bindings
