#include "input/virtual_pad.h"

#include "core/schema.h"

#include "engine/handle.h"

#include "base/hook.h"
#include "base/log.h"
#include "base/metrics.h"

#include <array>
#include <cstring>

namespace elmp::input::pad {

namespace {

#ifdef ELMP_DEV
constexpr bool kDev = true;
#else
constexpr bool kDev = false;
#endif

constexpr std::size_t kMax = 16;

struct Entry {
    std::int64_t slot = 0;
    ue::UObject* component = nullptr; // match key only; validated through `handle`
    ObjHandle handle, pawn, controller;
    PadState state;
    std::uint64_t frames = 0;
    std::uint32_t presses = 0;
};

std::array<Entry, kMax> g_entries{};
std::size_t g_count = 0;
bool g_player1 = false;

// Cached at attach (schema::script() is resolved by then).
std::int32_t g_snapshot_offset = -1;
reflect::PropRef g_pawn_controller, g_controller_pawn;

using Consume = std::uintptr_t(__fastcall*)(ue::UObject*, void*);
Consume g_original = nullptr;

metrics::Counter& c_dropped = metrics::counter("core.pad_dropped");
metrics::Counter& c_writes = metrics::counter("core.pad_writes");

void remove_at(std::size_t i) {
    g_entries[i] = g_entries[g_count - 1];
    g_entries[--g_count] = Entry{};
}

bool still_bound(const Entry& e, ue::UObject* component) {
    if (e.handle.get() != component) return false;
    auto* p = e.pawn.get();
    auto* c = e.controller.get();
    return p && c && g_pawn_controller.read<ue::UObject*>(p) == c && g_controller_pawn.read<ue::UObject*>(c) == p;
}

std::uintptr_t __fastcall consume(ue::UObject* component, void* parameter) {
    const auto result = g_original(component, parameter);
    if (!g_count) return result;
    for (std::size_t i = 0; i < g_count; ++i) {
        Entry& e = g_entries[i];
        if (e.component != component) continue;
        if (!still_bound(e, component)) {
            ++c_dropped;
            remove_at(i);
            return result;
        }
        const InputSnapshot snap = e.state.consume();
        std::memcpy(reinterpret_cast<std::uint8_t*>(component) + g_snapshot_offset, &snap, sizeof(snap));
        e.presses |= snap.flags;
        ++e.frames;
        ++c_writes;
        return result;
    }
    return result;
}

Entry* find(std::int64_t slot) {
    for (std::size_t i = 0; i < g_count; ++i)
        if (g_entries[i].slot == slot) return &g_entries[i];
    return nullptr;
}

Entry* live(std::int64_t slot) {
    Entry* e = find(slot);
    return e && e->handle.valid() ? e : nullptr;
}

} // namespace

std::string attach(std::int64_t slot, ue::UObject* pawn, ue::UObject* controller) {
    if (slot == 1 && !(kDev && g_player1)) return "slot 1 input injection is disabled (Dev builds only, explicit `elmp pad allow1 on`)";
    const auto* s = core::schema::script();
    if (!s) return "core schema unresolved";
    if (!pawn || !controller) return "slot has no possessed pawn";
    if (!pawn->IsA(static_cast<ue::UClass*>(s->zenith_character_class.get()))) return "expected a Zenith character";
    auto* component = s->zc_input.object(pawn);
    if (!component || s->zic_pawn_owner.object(component) != pawn) return "input component missing or not owned by the pawn";
    if (!g_original && !hook::install_manifest("InputConsume", reinterpret_cast<void*>(&consume), reinterpret_cast<void**>(&g_original)))
        return "InputConsume hook refused (see preflight)";
    g_snapshot_offset = s->zic_snapshot.offset;
    g_pawn_controller = s->pawn_controller;
    g_controller_pawn = s->controller_pawn;
    if (Entry* old = find(slot)) remove_at(static_cast<std::size_t>(old - g_entries.data()));
    if (g_count == kMax) return "too many virtual pads";
    g_entries[g_count++] = Entry{slot, component, ObjHandle::of(component), ObjHandle::of(pawn), ObjHandle::of(controller), {}, 0, 0};
    return {};
}

void detach(std::int64_t slot) {
    Entry* e = find(slot);
    if (!e) return;
    if (auto* component = e->handle.get()) {
        const InputSnapshot empty{};
        std::memcpy(reinterpret_cast<std::uint8_t*>(component) + g_snapshot_offset, &empty, sizeof(empty));
    }
    remove_at(static_cast<std::size_t>(e - g_entries.data()));
}

void forget_all() {
    g_entries.fill(Entry{});
    g_count = 0;
}

std::string set(std::int64_t slot, double x, double y, std::int64_t raw) {
    Entry* e = live(slot);
    return e ? e->state.set(x, y, raw) : "slot is not attached to a virtual pad";
}

std::string axes(std::int64_t slot, double x, double y) {
    Entry* e = live(slot);
    return e ? e->state.set_axes(x, y) : "slot is not attached to a virtual pad";
}

std::string hold(std::int64_t slot, std::uint32_t bits, bool down) {
    Entry* e = live(slot);
    return e ? e->state.hold(bits, down) : "slot is not attached to a virtual pad";
}

std::string tap(std::int64_t slot, std::uint32_t bits) {
    Entry* e = live(slot);
    return e ? e->state.tap(bits) : "slot is not attached to a virtual pad";
}

Info info(std::int64_t slot) {
    const Entry* e = live(slot);
    if (!e) return {};
    return {true, e->state.x(), e->state.y(), e->state.raw(), e->presses, e->frames};
}

std::uint32_t take_presses(std::int64_t slot) {
    Entry* e = live(slot);
    if (!e) return 0;
    const auto p = e->presses;
    e->presses = 0;
    return p;
}

std::string allow_player1(bool on) {
    if (on && !kDev) return "slot 1 input injection is only available in Dev builds";
    g_player1 = on;
    if (!on) detach(1);
    return {};
}

bool player1_allowed() { return g_player1; }

std::size_t attached() { return g_count; }

} // namespace elmp::input::pad
