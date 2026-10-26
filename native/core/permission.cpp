#include "core/permission.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "base/json.h"
#include "base/log.h"
#include "base/metrics.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <unordered_map>
#include <vector>

namespace elmp::core::permission {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(Gate::Count)> kNames{"travel", "rest", "trigger.story", "trigger.world", "pickup", "interact", "save"};
std::array<bool, static_cast<std::size_t>(Gate::Count)> kDefaults{false, false, false, true, true, true, false}; // data/gates.json may override

// Gate targets per registry bucket (an object can be in several buckets: a bench is also an Interactable;
// it is ignored when any of its gates is closed).
struct KindGate {
    registry::Kind kind;
    Gate gate; // Trigger: resolved per class (story / world)
};
// pickup is not a target bucket: drops spawn already overlapping the seat (the collectable's own overlap
// update ignores the seat's MoveIgnoreActors), so it closes the seat's UAutoCollectComponent sphere instead.
constexpr std::array<KindGate, 4> kKinds{{{registry::Kind::TravelVolume, Gate::Travel},
                                          {registry::Kind::RestingPoint, Gate::Rest},
                                          {registry::Kind::Interactable, Gate::Interact},
                                          {registry::Kind::Trigger, Gate::TriggerStory}}};

std::map<std::string, Gate, std::less<>> g_trigger_policy; // blueprint class name → story / world
std::unordered_map<std::uint16_t, Gate> g_trigger_class;    // registry class id → resolved gate
std::vector<std::string> g_unlisted;                         // reported once each
std::string g_gates_status = "data/gates.json not loaded (built-in defaults; every trigger counts as story)";

Gate trigger_gate(const registry::Entry& e) {
    if (const auto it = g_trigger_class.find(e.class_id); it != g_trigger_class.end()) return it->second;
    Gate g = Gate::TriggerStory;
    bool listed = false;
    std::string most_derived;
    if (auto* o = e.handle.get())
        for (ue::UStruct* s = o->GetClassPrivate(); s && !listed; s = s->GetSuperStruct()) {
            const auto name = log::utf8(s->GetName());
            if (most_derived.empty()) most_derived = name;
            if (const auto p = g_trigger_policy.find(name); p != g_trigger_policy.end()) g = p->second, listed = true;
        }
    if (!listed && std::find(g_unlisted.begin(), g_unlisted.end(), most_derived) == g_unlisted.end()) {
        g_unlisted.push_back(most_derived);
        log::warn("gates: trigger class {} is not in data/gates.json; treated as story (PX ignore it)", most_derived);
    }
    g_trigger_class[e.class_id] = g;
    return g;
}

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
    ObjHandle sphere;           // UAutoCollectComponent.SphereComponent of `pawn`
    std::int8_t sphere_on = -1; // last SetGenerateOverlapEvents value applied (-1 = none)
    std::array<std::uint64_t, kKinds.size()> versions;
    Rec() { versions.fill(~0ull); }
    bool dirty = true;
};

std::map<std::int64_t, Rec> g_recs; // slot 1 = P1 (rule-2 door hold only)
Grant g_grant = nullptr;
bool g_p1_hold = false;
bool g_p1_presentation = false; // guest process: P1 is a presentation of the host seat (FR-NET-3d)
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

ue::UObject* autocollect_sphere(const schema::Script& s, ue::UObject* pawn) {
    reflect::Frame f(s.actor_component_by_class);
    f.arg(0, s.autocollect_class.get());
    if (!f.invoke(pawn)) return nullptr;
    auto* comp = f.ret<ue::UObject*>();
    return ue::alive(comp) ? s.autocollect_sphere.object(comp) : nullptr;
}

bool want_ignore(std::int64_t slot, Gate g, const registry::Entry& e) {
    if (slot == 1) return g_p1_presentation || (g == Gate::Travel && g_p1_hold && e.handle.index != g_p1_open);
    if (g == Gate::TriggerStory) g = trigger_gate(e);
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
        r.sphere = ObjHandle::of(slot == 1 && !g_p1_presentation ? nullptr : autocollect_sphere(*s, pawn));
        r.sphere_on = -1;
        r.dirty = true;
    }
    if ((slot != 1 || g_p1_presentation || r.sphere_on == 0) && r.dirty) {
        const std::int8_t on = slot == 1 ? (g_p1_presentation ? 0 : 1) : (allows(slot, Gate::Pickup) ? 1 : 0);
        auto* sphere = r.sphere.get();
        if (sphere && on != r.sphere_on && !(r.sphere_on < 0 && on)) { // a fresh sphere already overlaps
            reflect::Frame f(s->prim_set_overlaps);
            f.arg_bool(0, on != 0);
            f.invoke(sphere);
            ++c_ignore_calls;
        }
        r.sphere_on = on;
    }
    bool changed = r.dirty;
    for (std::size_t k = 0; k < kKinds.size(); ++k) {
        const auto v = registry::version(kKinds[k].kind);
        changed |= r.versions[k] != v;
        r.versions[k] = v;
    }
    if (!changed) return;
    std::erase_if(r.applied, [](const auto& e) { return !ObjHandle{e.first, e.second.serial}.valid(); }); // objects of unloaded areas
    // Desired state per target: ignored when any gate of any bucket it belongs to is closed. P1 only has
    // the rule-2 door hold.
    std::unordered_map<std::int32_t, std::pair<ObjHandle, bool>> want;
    for (const auto& kg : kKinds) {
        if (slot == 1 && kg.gate != Gate::Travel && !g_p1_presentation) continue;
        for (const auto& e : registry::bucket(kg.kind)) {
            auto& w = want[e.handle.index];
            w.first = e.handle;
            w.second = w.second || want_ignore(slot, kg.gate, e);
        }
    }
    for (const auto& [index, w] : want) {
        auto* target = w.first.get();
        if (!target) continue;
        const bool ignore = w.second;
        auto& t = r.applied[index];
        if (t.serial == w.first.serial && t.ignored == ignore) continue;
        if (t.serial != w.first.serial && !ignore) { // never ignored on this pawn: nothing to revoke
            t = {w.first.serial, false};
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
        t = {w.first.serial, ignore};
    }
    r.dirty = false;
}

void on_teardown() {
    g_recs.clear();
    g_trigger_class.clear(); // class ids are per registry lifetime; blueprint classes may reload
}

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

void p1_presentation(bool on) {
    if (on == g_p1_presentation) return;
    g_p1_presentation = on;
    auto& r = g_recs[1];
    r.managed = on || g_p1_hold || !r.applied.empty();
    r.pawn.reset(); // re-resolve the sphere for the new mode
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

std::string load(const std::filesystem::path& file) {
    std::string err;
    const auto root = json_io::load(file, 1, &err);
    if (!root) return g_gates_status = std::format("data/gates.json unusable ({}); built-in defaults, every trigger counts as story", err);
    if (const auto* d = root->get("defaults"); d && d->is_object())
        for (const auto& [name, v] : d->obj) {
            Gate g;
            if (!gate_named(name, g) || !v.is_bool()) return g_gates_status = "data/gates.json: bad default " + name;
            kDefaults[static_cast<std::size_t>(g)] = v.b;
        }
    if (const auto* u = root->get("unlisted"); u && u->string_or("story") != "story") return g_gates_status = "data/gates.json: unlisted must be story (§7.13.2)";
    if (const auto* t = root->get("triggers"); t && t->is_object())
        for (const auto& [cls, v] : t->obj) {
            const auto kind = v.string_or("");
            if (kind != "story" && kind != "world") return g_gates_status = "data/gates.json: bad trigger kind for " + cls;
            g_trigger_policy[cls] = kind == "world" ? Gate::TriggerWorld : Gate::TriggerStory;
        }
    g_gates_status = std::format("data/gates.json: {} trigger classes", g_trigger_policy.size());
    return {};
}

std::string describe() {
    std::string out = g_gates_status + "\n";
    if (!g_unlisted.empty()) {
        out += "unlisted trigger classes (treated as story):";
        for (const auto& c : g_unlisted) out += " " + c;
        out += "\n";
    }
    for (const auto& [slot, r] : g_recs) {
        int ignored = 0;
        for (const auto& [i, t] : r.applied) ignored += t.ignored;
        std::string flags;
        for (std::size_t g = 0; g < kNames.size(); ++g) flags += std::format(" {}={}", kNames[g], slot == 1 ? "-" : (allows(slot, static_cast<Gate>(g)) ? "on" : "off"));
        const char* sphere = !r.sphere.valid() ? "none" : (r.sphere_on ? "on" : "off");
        out += std::format("{}: pawn={} comps={} targets={} ignored={} collect-sphere={}{}\n", slot == 1 ? "P1" : std::format("P{}", slot), r.pawn.valid() ? "live" : "none",
                           r.comps.size(), r.applied.size(), ignored, slot == 1 ? "-" : sphere, slot == 1 ? std::format(" rule-2 hold={} open={}", g_p1_hold, g_p1_open) : flags);
    }
    return g_recs.empty() ? out + "no gated seats\n" : out;
}

void install() { lifecycle::on_teardown(on_teardown); }

} // namespace elmp::core::permission
