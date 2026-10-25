#pragma once

// Overlay models (§3 / §7.7, M23): plain fixed-size structs. The game thread (party/hud_feed, menu_ctrl,
// manager) builds a staging copy and publishes it through a seqlock; the render thread (Present) copies the
// latest one out and only draws. User actions in ELMP windows go back as Requests through an SPSC ring and
// are executed on the game thread. No UObject pointers ever cross: only numbers and UTF-8 text.

#include "base/seqlock.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace elmp::overlay {

template <std::size_t N>
struct Text {
    char s[N]{};
    void set(std::string_view v) {
        std::size_t n = std::min(v.size(), N - 1);
        while (n > 0 && n < v.size() && (static_cast<unsigned char>(v[n]) & 0xC0) == 0x80) --n; // keep whole UTF-8 sequences
        std::memcpy(s, v.data(), n);
        s[n] = 0;
    }
    const char* c_str() const { return s; }
    bool empty() const { return !s[0]; }
    std::string_view view() const { return s; }
};

inline constexpr int kMaxSeats = 16;
inline constexpr int kActions = 15; // input::bindings::Count

// ---- HUD (per local seat: head anchor + right-side panel) ----
struct HudSkill {
    Text<12> key;
    Text<40> icon;      // icon id (data/icons/<id>.png); empty = nothing equipped
    int remaining = -1; // ceil(GetRemainingCastValue); -1 unknown / unlimited
    std::int8_t limit = -1; // ECommandSummonLimitType; -1 = action unknown
    float cooldown = 0;  // GetCooldownTime / GetRecastTime, 0..1
};
struct HudSeat {
    std::int64_t slot = 0;
    std::uint8_t team = 1; // 1 friendly, 2 enemy
    float hp = 0, max_hp = 0, mp = 0, max_mp = 0, special_cost = 100;
    int heals = 0, max_heals = 0;
    std::uint8_t summon_set = 1;
    bool special_unlocked = false, anchored = false, collapsed = false, alive = true;
    float ax = 0, ay = 0; // head anchor, viewport pixels
    Text<40> prompt, toast;
    double toast_until = 0; // clock seconds
    HudSkill skills[3];
};
struct Hud {
    bool visible = false;
    int count = 0;
    HudSeat seats[kMaxSeats];
};

// ---- PX equipment menu ----
struct MenuItem {
    Text<32> id;
    Text<48> name;
    Text<48> icon;
    Text<320> desc;
    std::int16_t level = 0;
    std::int8_t slots = 0;
    bool main = false, equipped = false;
};
inline constexpr int kMenuColumns = 6, kMaxSpirits = 32, kMaxPassives = 48;
struct Menu {
    bool open = false;
    std::int64_t slot = 0;
    int page = 0; // 0 spirits, 1 relics
    bool in_list = false;
    int set = 0, cell = 0, cursor = 0;
    bool can_change = false, set2_locked = false;
    Text<64> reason;
    Text<32> sets[2][3]; // spirit id per set / cell (ATTACK, SUMMON_1, SUMMON_2)
    int nspirits = 0, npassives = 0;
    MenuItem spirits[kMaxSpirits];
    MenuItem passives[kMaxPassives];
    int used_slots = 0, max_slots = 0;
    Text<96> message;
    double message_until = 0;
    Text<16> confirm, back, context, prev, next; // fixed menu keys (labels for the seat's device)
    Text<16> cells[3];                           // the seat's ATTACK / SUMMON_1 / SUMMON_2 key
};

// ---- ELMP manager ----
struct ManagerSeat {
    std::int64_t slot = 0;
    bool alive = true;
    int skin = 1;
    std::uint8_t team = 1;
    Text<32> keys[kActions], pads[kActions];
};
struct ManagerDevice {
    Text<96> id;
    Text<64> name;
    bool keyboard = false, connected = false, active = false;
    std::int64_t assigned = 0; // 0 = unassigned (P1)
};
struct Rules {
    int map_rule = 3, pvp_damage_pct = 10, knockback = 0;
    bool equip_anywhere = false, recall = true, camera_framing = true;
    int recall_distance = 2500, recall_delay_ms = 2000, recall_fall_depth = 2000;
};
struct Manager {
    bool button = false, open = false, muted = false;
    int nseats = 0, ndevices = 0;
    ManagerSeat seats[kMaxSeats];
    ManagerDevice devices[12];
    bool capturing = false, capture_keyboard = false;
    std::int64_t capture_slot = 0;
    int capture_action = 0;
    Rules rules;
    Text<16> storage_id;
    Text<240> storage_message;
    bool storage_blocked = false, storage_can_reuse = false;
    Text<128> message;
    double message_until = 0;
};

enum class Req : std::uint8_t {
    ManagerOpen, ManagerClose, AddPlayer, RemovePlayer, Skin, Team, Assign, Mute, Capture, CancelCapture, ResetBindings, Rule,
    StorageNew, StorageReuse,
};
struct Request {
    Req kind{};
    std::int64_t slot = 0;
    double value = 0;
    bool keyboard = false;
    int action = 0;
    Text<96> text; // device id / settings key
};

Seqlock<Hud>& hud();
Seqlock<Menu>& menu();
Seqlock<Manager>& manager();
bool request(const Request& r); // render thread → game thread
bool next_request(Request& r);  // game thread

} // namespace elmp::overlay
