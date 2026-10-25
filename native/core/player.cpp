#include "core/player.h"

#include "core/schema.h"
#include "core/vitals.h"

#include "input/virtual_pad.h"

#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/events.h"
#include "base/log.h"
#include "base/metrics.h"

#include <elmp/build_info.h>

#include <Unreal/Core/Containers/Array.hpp>

#include <format>
#include <map>

namespace elmp::core {

using namespace RC::Unreal;

namespace {

struct Rec {
    ObjHandle pc, pawn;
};

std::map<std::int64_t, Rec> g_pool;
metrics::Counter& c_lost = metrics::counter("core.pool_lost");

constexpr const wchar_t* kTag = L"ELMP";
constexpr std::wstring_view kSlotPrefix = L"ELMP.Slot.";

TArray<FName>* tags_of(ue::UObject* actor) {
    const auto* s = schema::script();
    return s && actor ? s->actor_tags.at<TArray<FName>>(actor) : nullptr;
}

void on_teardown() {
    // Controllers die with the world: drop handles, release every lease (state rebuilt per world, M2).
    input::pad::forget_all();
    for (auto& [slot, rec] : g_pool) events::bus().push(events::Type::SeatLeave, static_cast<std::int32_t>(slot), 1);
    g_pool.clear();
    for (const auto& [slot, owner] : leases().leases()) leases().release({owner, owner_for(slot).kind}, slot);
    schema::on_teardown();
}

} // namespace

slots::Table& leases() {
    static slots::Table t(build::dev);
    return t;
}

slots::Owner owner_for(std::int64_t slot, std::string_view name) {
    if (slot >= slots::kTestFirst && slot <= slots::kTestLast) return {std::string(name == "ELMP" ? "console" : name), slots::OwnerKind::DevTool};
    return {std::string(name), slots::OwnerKind::Elmp};
}

ue::UObject* controller(std::int64_t slot) {
    if (slot == slots::kP1) return world::p1_controller();
    auto it = g_pool.find(slot);
    if (it == g_pool.end()) return nullptr;
    if (auto* pc = it->second.pc.get()) return pc;
    ++c_lost;
    log::warn("slot {}: pooled controller disappeared", slot);
    g_pool.erase(it);
    return nullptr;
}

ue::UObject* pawn(std::int64_t slot) {
    const auto* s = schema::script();
    auto* pc = controller(slot);
    if (!s || !pc) return nullptr;
    auto* p = s->controller_pawn.object(pc);
    if (!p || s->pawn_controller.object(p) != pc) return nullptr;
    return p;
}

bool is_elmp_controller(const ue::UObject* pc) {
    if (!pc) return false;
    for (const auto& [slot, rec] : g_pool)
        if (rec.pc.get() == pc) return true;
    return false;
}

std::vector<std::int64_t> slots_in_world() {
    std::vector<std::int64_t> out;
    for (const auto& [slot, rec] : g_pool)
        if (rec.pc.valid()) out.push_back(slot);
    return out;
}

std::size_t active() {
    std::size_t n = 0;
    for (const auto& [slot, rec] : g_pool)
        if (rec.pawn.valid()) ++n;
    return n;
}

void tag(ue::UObject* actor, std::int64_t slot) {
    auto* tags = tags_of(actor);
    if (!tags) return;
    const FName marker(kTag, FNAME_Add);
    const FName slot_tag(std::format(L"ELMP.Slot.{}", slot).c_str(), FNAME_Add);
    bool has_marker = false, has_slot = false;
    for (const FName& n : *tags) {
        has_marker |= n == marker;
        has_slot |= n == slot_tag;
    }
    if (!has_marker) tags->Add(marker);
    if (!has_slot) tags->Add(slot_tag);
}

std::int64_t slot_of(ue::UObject* actor) {
    auto* tags = tags_of(actor);
    if (!tags || tags->Num() > 256) return 0;
    const FName marker(kTag, FNAME_Add);
    bool marked = false;
    std::int64_t slot = 0;
    for (const FName& n : *tags) {
        if (n == marker) {
            marked = true;
            continue;
        }
        const auto text = n.ToString();
        if (std::wstring_view(text).starts_with(kSlotPrefix)) slot = std::wcstoll(text.c_str() + kSlotPrefix.size(), nullptr, 10);
    }
    return marked ? slot : 0;
}

void remember(std::int64_t slot, ue::UObject* pc) { g_pool[slot].pc = ObjHandle::of(pc); }

void set_pawn(std::int64_t slot, ue::UObject* p) {
    if (auto it = g_pool.find(slot); it != g_pool.end()) it->second.pawn = ObjHandle::of(p);
}

std::optional<Vec3> location(ue::UObject* actor) {
    const auto* s = schema::script();
    if (!s || !actor) return std::nullopt;
    auto* root = s->actor_root.object(actor);
    if (!root) return std::nullopt;
    return s->scene_rel_location.read<Vec3>(root);
}

State state(std::int64_t slot) {
    State st;
    const auto* s = schema::script();
    auto* p = pawn(slot);
    if (!s || !p) return st;
    st.valid = true;
    if (auto l = location(p)) st.pos = *l;
    const auto v = vitals::read(slot);
    st.alive = v.alive;
    st.hp = v.hp, st.max_hp = v.max_hp, st.mp = v.mp, st.max_mp = v.max_mp, st.heals = v.heals, st.max_heals = v.max_heals;
    if (auto* cq = s->zc_command_queue.object(p)) {
        reflect::Frame f(s->cq_has_command);
        if (f.invoke(cq)) st.busy = f.ret_bool();
    }
    if (const auto* bp = schema::blueprint()) {
        if (auto* spine = bp->lily_spine.object(p)) {
            reflect::Frame cur(s->sp_get_current);
            cur.arg(0, 0);
            if (cur.invoke(spine)) {
                if (auto* entry = cur.ret<ue::UObject*>(); ue::alive(entry)) {
                    reflect::Frame name(s->te_animation_name);
                    if (name.invoke(entry)) st.animation = log::utf8(name.ret_string());
                    reflect::Frame t(s->te_track_time);
                    if (t.invoke(entry)) st.track_time = t.ret<float>();
                }
            }
        }
    }
    return st;
}

void install() { lifecycle::on_teardown(on_teardown); }

std::string describe() {
    std::string out;
    for (const auto& [slot, rec] : g_pool) {
        const auto owner = leases().owner_of(slot);
        out += std::format("slot {}: pc={} pawn={} lease={}\n", slot, rec.pc.valid() ? "live" : "gone", rec.pawn.valid() ? "live" : "none", owner ? *owner : "-");
    }
    return out.empty() ? "no ELMP slots in this world\n" : out;
}

} // namespace elmp::core
