#include "core/permission.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "base/metrics.h"

#include <array>
#include <format>
#include <map>
#include <unordered_map>
#include <vector>

namespace elmp::core::permission {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(Gate::Count)> kNames{"travel", "rest", "trigger.story", "trigger.world", "pickup", "interact", "save"};
constexpr std::array<bool, static_cast<std::size_t>(Gate::Count)> kDefaults{false, false, false, true, true, true, false};

struct Target {
    std::int32_t serial = 0;
    bool ignored = false;
};

struct Rec {
    std::array<bool, static_cast<std::size_t>(Gate::Count)> allowed = kDefaults;
    bool managed = false;
    ObjHandle pawn;
    std::vector<ObjHandle> comps;
    std::unordered_map<std::int32_t, Target> applied; // target object index → state
    std::array<std::uint64_t, 2> versions{~0ull, ~0ull};
    bool dirty = true;
};

std::map<std::int64_t, Rec> g_recs; // slot 1 = P1 (rule-2 door hold only)
Grant g_grant = nullptr;
bool g_p1_hold = false;
std::int32_t g_p1_open = 0;
metrics::Counter& c_ignore_calls = metrics::counter("gates.ignore_calls");

std::vector<ObjHandle> primitives(const schema::Script& s, ue::UObject* pawn) {
    std::vector<ObjHandle> out;
    reflect::Frame f(s.actor_components_by_class);
    f.arg(0, s.primitive_class.get());
    if (!f.invoke(pawn)) return out;
    const auto* arr = static_cast<const reflect::RawArray*>(f.raw(s.actor_components_by_class.ret));
    if (!arr || arr->num < 0 || arr->num > 64) return out;
    const auto* items = reinterpret_cast<ue::UObject* const*>(arr->data);
    for (int i = 0; i < arr->num; ++i)
        if (ue::alive(items[i])) out.push_back(ObjHandle::of(items[i]));
    return out;
}

bool want_ignore(std::int64_t slot, const Rec& r, Gate g, const registry::Entry& e) {
    if (slot == 1) return g == Gate::Travel && g_p1_hold && e.handle.index != g_p1_open;
    return !allows(slot, g);
}

void apply(std::int64_t slot, Rec& r) {
    const auto* s = schema::script();
    if (!s) return;
    auto* pawn = slot == 1 ? world::p1_pawn() : core::pawn(slot);
    if (!pawn) {
        r.pawn.reset();
        return;
    }
    if (r.pawn.get() != pawn) { // new pawn: nothing applied yet
        r.pawn = ObjHandle::of(pawn);
        r.comps = primitives(*s, pawn);
        r.applied.clear();
        r.dirty = true;
    }
    const std::array<std::pair<Gate, registry::Kind>, 2> kinds{{{Gate::Travel, registry::Kind::TravelVolume}, {Gate::Rest, registry::Kind::RestingPoint}}};
    for (std::size_t k = 0; k < kinds.size(); ++k) {
        const auto [gate, kind] = kinds[k];
        const auto v = registry::version(kind);
        if (!r.dirty && r.versions[k] == v) continue;
        r.versions[k] = v;
        std::erase_if(r.applied, [](const auto& e) { return !ObjHandle{e.first, e.second.serial}.valid(); }); // doors of unloaded areas
        for (const auto& e : registry::bucket(kind)) {
            auto* target = e.handle.get();
            if (!target) continue;
            const bool ignore = want_ignore(slot, r, gate, e);
            auto& t = r.applied[e.handle.index];
            if (t.serial == e.handle.serial && t.ignored == ignore) continue;
            if (t.serial != e.handle.serial && !ignore) { // never ignored on this pawn: nothing to revoke
                t = {e.handle.serial, false};
                continue;
            }
            for (const auto& ch : r.comps) {
                if (auto* comp = ch.get()) {
                    reflect::Frame f(s->prim_ignore_actor);
                    f.arg(0, target).arg_bool(1, ignore);
                    f.invoke(comp);
                    ++c_ignore_calls;
                }
            }
            t = {e.handle.serial, ignore};
        }
    }
    r.dirty = false;
}

void on_teardown() { g_recs.clear(); }

} // namespace

const char* gate_name(Gate g) noexcept { return kNames[static_cast<std::size_t>(g)]; }

bool gate_named(std::string_view name, Gate& out) {
    for (std::size_t i = 0; i < kNames.size(); ++i)
        if (name == kNames[i]) {
            out = static_cast<Gate>(i);
            return true;
        }
    return false;
}

void set(std::int64_t slot, Gate g, bool allowed) {
    if (slot < 2) return;
    auto& r = g_recs[slot];
    r.allowed[static_cast<std::size_t>(g)] = allowed;
    r.dirty = true;
    apply(slot, r);
}

bool allows(std::int64_t slot, Gate g) {
    if (slot == 1) return true;
    if (g_grant && g_grant(slot, g)) return true;
    const auto it = g_recs.find(slot);
    return it == g_recs.end() ? kDefaults[static_cast<std::size_t>(g)] : it->second.allowed[static_cast<std::size_t>(g)];
}

void set_grant(Grant grant) {
    g_grant = grant;
    for (auto& [slot, r] : g_recs) r.dirty = true;
}

void hold_p1_doors(bool hold, std::int32_t open_door) {
    if (hold == g_p1_hold && open_door == g_p1_open) return;
    g_p1_hold = hold;
    g_p1_open = open_door;
    auto& r = g_recs[1];
    r.managed = hold || !r.applied.empty();
    r.dirty = true;
    apply(1, r);
}

void track(std::int64_t slot) {
    auto& r = g_recs[slot];
    r.managed = true;
    r.dirty = true;
    apply(slot, r);
}

void forget(std::int64_t slot) { g_recs.erase(slot); }

void tick() {
    for (auto& [slot, r] : g_recs)
        if (r.managed) apply(slot, r);
}

std::string describe() {
    std::string out;
    for (const auto& [slot, r] : g_recs) {
        int ignored = 0;
        for (const auto& [i, t] : r.applied) ignored += t.ignored;
        std::string flags;
        for (std::size_t g = 0; g < kNames.size(); ++g) flags += std::format(" {}={}", kNames[g], slot == 1 ? "-" : (allows(slot, static_cast<Gate>(g)) ? "on" : "off"));
        out += std::format("{}: pawn={} comps={} targets={} ignored={}{}\n", slot == 1 ? "P1" : std::format("P{}", slot), r.pawn.valid() ? "live" : "none", r.comps.size(),
                           r.applied.size(), ignored, slot == 1 ? std::format(" rule-2 hold={} open={}", g_p1_hold, g_p1_open) : flags);
    }
    return out.empty() ? "no gated seats\n" : out;
}

void install() { lifecycle::on_teardown(on_teardown); }

} // namespace elmp::core::permission
