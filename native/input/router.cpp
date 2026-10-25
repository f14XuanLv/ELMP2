#include "input/router.h"

#include "base/metrics.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <mutex>

namespace elmp::input::router {

namespace {

constexpr std::size_t kMaxPending = 64;

struct RawKey {
    std::string keyboard;
    std::uint32_t vk;
    bool down;
};

std::mutex g_mutex;
std::map<std::string, std::int64_t> g_assign;
std::set<std::int64_t> g_seats;
std::map<std::string, std::bitset<256>> g_keys;
std::map<std::string, std::bitset<256>> g_latched; // key-downs not yet seen by a seat read (sub-frame taps)
std::deque<RawKey> g_pending;
std::map<std::int64_t, bindings::Bindings> g_bindings;
std::map<std::int64_t, std::uint32_t> g_previous;
bool g_muted = false;
std::atomic<bool> g_active{false};
std::array<std::atomic<bool>, kPads> g_connected{};
metrics::Counter& c_swallowed = metrics::counter("input.keys_swallowed");

std::int64_t owner_locked(const std::string& device) {
    if (g_muted) return 0;
    const auto it = g_assign.find(device);
    if (it == g_assign.end() || it->second == kP1) return kP1;
    return g_seats.contains(it->second) ? it->second : kP1;
}

void refresh_active_locked() {
    bool on = g_muted;
    for (const auto& [device, slot] : g_assign) on |= slot != kP1 && g_seats.contains(slot);
    g_active.store(on, std::memory_order_release);
}

template <class T>
T stronger(T a, T b) {
    return std::abs(static_cast<int>(b)) > std::abs(static_cast<int>(a)) ? b : a;
}

} // namespace

std::string pad_id(int index) { return "xinput:" + std::to_string(index); }

std::optional<int> pad_index(std::string_view id) {
    if (!id.starts_with("xinput:") || id.size() != 8) return std::nullopt;
    const int n = id[7] - '0';
    return n >= 0 && n < kPads ? std::optional(n) : std::nullopt;
}

void assign(const std::string& device, std::int64_t slot) {
    std::scoped_lock lock(g_mutex);
    if (slot <= kP1) g_assign.erase(device);
    else g_assign[device] = slot;
    refresh_active_locked();
}

std::map<std::string, std::int64_t> assignments() {
    std::scoped_lock lock(g_mutex);
    return g_assign;
}

void set_assignments(std::map<std::string, std::int64_t> table) {
    std::scoped_lock lock(g_mutex);
    g_assign = std::move(table);
    std::erase_if(g_assign, [](const auto& e) { return e.second <= kP1; });
    refresh_active_locked();
}

void set_seats(const std::set<std::int64_t>& seats) {
    std::scoped_lock lock(g_mutex);
    g_seats = seats;
    std::erase_if(g_previous, [&](const auto& e) { return !seats.contains(e.first); });
    refresh_active_locked();
}

std::int64_t owner(const std::string& device) {
    std::scoped_lock lock(g_mutex);
    return owner_locked(device);
}

bool has_devices(std::int64_t slot) {
    std::scoped_lock lock(g_mutex);
    if (g_muted || slot == kP1 || !g_seats.contains(slot)) return false;
    return std::any_of(g_assign.begin(), g_assign.end(), [&](const auto& e) { return e.second == slot; });
}

bool has_pad(std::int64_t slot) {
    std::scoped_lock lock(g_mutex);
    return std::any_of(g_assign.begin(), g_assign.end(), [&](const auto& e) { return e.second == slot && pad_index(e.first); });
}

void set_muted(bool m) {
    std::scoped_lock lock(g_mutex);
    g_muted = m;
    if (m) {
        g_keys.clear();
        g_latched.clear();
        g_pending.clear();
        g_previous.clear();
    }
    refresh_active_locked();
}

bool muted() {
    std::scoped_lock lock(g_mutex);
    return g_muted;
}

bool active() { return g_active.load(std::memory_order_acquire); }
std::uint64_t swallowed() { return c_swallowed.load(); }

void raw_key(const std::string& keyboard, std::uint32_t vk, bool down) {
    std::scoped_lock lock(g_mutex);
    if (g_muted) return;
    if (vk < 256) {
        g_keys[keyboard].set(vk, down);
        if (down) g_latched[keyboard].set(vk);
    }
    g_pending.push_back({keyboard, vk, down});
    if (g_pending.size() > kMaxPending) g_pending.pop_front();
}

bool legacy_allowed(std::uint32_t vk, bool down) {
    std::scoped_lock lock(g_mutex);
    if (g_muted) {
        g_pending.clear();
        ++c_swallowed;
        return false;
    }
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (it->vk != vk || it->down != down) continue;
        const bool allowed = owner_locked(it->keyboard) == kP1;
        g_pending.erase(g_pending.begin(), std::next(it));
        if (!allowed) ++c_swallowed;
        return allowed;
    }
    return true;
}

void clear_keys() {
    std::scoped_lock lock(g_mutex);
    g_keys.clear();
    g_latched.clear();
    g_pending.clear();
}

void set_bindings(std::int64_t slot, bindings::Bindings b) {
    std::scoped_lock lock(g_mutex);
    g_bindings[slot] = std::move(b);
}

bindings::Bindings bindings_of(std::int64_t slot) {
    std::scoped_lock lock(g_mutex);
    const auto it = g_bindings.find(slot);
    return it == g_bindings.end() ? bindings::defaults() : it->second;
}

std::map<std::int64_t, bindings::Bindings> all_bindings() {
    std::scoped_lock lock(g_mutex);
    return g_bindings;
}

bindings::Actions seat_actions(std::int64_t slot, PadReader read, const bindings::Bindings* fixed) {
    // Whole computation under the lock (the pad read does not touch the router); no copies per frame.
    std::scoped_lock lock(g_mutex);
    if (g_muted || slot == kP1 || !g_seats.contains(slot)) return {};
    const bindings::Bindings* b = fixed;
    if (!b) {
        const auto it = g_bindings.find(slot);
        b = it == g_bindings.end() ? &bindings::defaults() : &it->second;
    }
    bindings::Actions out;
    for (const auto& [device, assigned] : g_assign) {
        if (assigned != slot) continue;
        if (const auto i = pad_index(device)) {
            // Unplugged indices are never read on the game thread (~0.35 ms per XInputGetState, Dev2).
            if (!g_connected[*i]) continue;
            if (const auto s = read(*i)) out = bindings::merge(out, bindings::from_pad(*b, *s));
        } else if (const auto k = g_keys.find(device); k != g_keys.end()) {
            auto keys = k->second;
            if (!fixed)
                if (auto l = g_latched.find(device); l != g_latched.end()) {
                    keys |= l->second;
                    l->second.reset();
                }
            out = bindings::merge(out, bindings::from_keyboard(*b, keys));
        }
    }
    return out;
}

std::uint32_t pressed(std::int64_t slot, const bindings::Actions& now) {
    std::scoped_lock lock(g_mutex);
    auto& prev = g_previous[slot];
    const auto result = now.held & ~prev;
    prev = now.held;
    return result;
}

std::optional<gamepad::State> player1_pad(PadReader read) {
    std::array<int, kPads> mine{};
    std::size_t n = 0;
    {
        std::scoped_lock lock(g_mutex);
        if (g_muted) return std::nullopt;
        for (int i = 0; i < kPads; ++i)
            if (g_connected[i] && owner_locked(pad_id(i)) == kP1) mine[n++] = i;
    }
    std::optional<gamepad::State> out;
    for (std::size_t k = 0; k < n; ++k) {
        const auto s = read(mine[k]);
        if (!s) continue;
        if (!out) {
            out = *s;
            continue;
        }
        out->buttons |= s->buttons;
        out->left_trigger = std::max(out->left_trigger, s->left_trigger);
        out->right_trigger = std::max(out->right_trigger, s->right_trigger);
        out->thumb_lx = stronger(out->thumb_lx, s->thumb_lx);
        out->thumb_ly = stronger(out->thumb_ly, s->thumb_ly);
        out->thumb_rx = stronger(out->thumb_rx, s->thumb_rx);
        out->thumb_ry = stronger(out->thumb_ry, s->thumb_ry);
    }
    return out;
}

void set_connected(int index, bool c) {
    if (index >= 0 && index < kPads) g_connected[index] = c;
}

bool connected(int index) { return index >= 0 && index < kPads && g_connected[index]; }

} // namespace elmp::input::router
