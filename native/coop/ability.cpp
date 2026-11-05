#include "coop/ability.h"

#include "engine/handle.h"
#include "engine/reflect.h"

#include "base/hook.h"
#include "base/log.h"

#include <cstring>
#include <algorithm>
#include <format>
#include <limits>
#include <unordered_map>
#include <vector>

namespace elmp::coop::ability {

namespace {

using RC::Unreal::UObject;

struct Layout {
    reflect::PropRef cls, params, max_time, damage_rate, offset, spawn_bone, custom_spawn, follow_spawn, attach_bone, custom_attach, follow_attach, ffa;
    bool ok = false, tried = false;
};
Layout g_l;

const Layout& layout() {
    if (g_l.tried) return g_l;
    g_l.tried = true;
    reflect::Resolver r("AbilityDescription", static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.AbilityDescription")));
    g_l.cls = r.prop(L"AbilityClass", reflect::Kind::Any, 8);
    g_l.params = r.prop(L"AbilityParameters", reflect::Kind::Struct, 8);
    reflect::Resolver pr("AbilityParams", static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.AbilityParams")));
    g_l.max_time = pr.prop(L"MaxExecutionTimeOverride", reflect::Kind::Float, 4);
    g_l.damage_rate = pr.prop(L"DamageRate", reflect::Kind::Float, 4);
    g_l.offset = r.prop(L"Offset", reflect::Kind::Struct, 48);
    g_l.spawn_bone = r.prop(L"SpawnBone", reflect::Kind::Any, 1);
    g_l.custom_spawn = r.prop(L"CustomSpawnBone", reflect::Kind::Name);
    g_l.follow_spawn = r.prop(L"bFollowSpawnBoneRotation", reflect::Kind::Bool);
    g_l.attach_bone = r.prop(L"AttachBone", reflect::Kind::Any, 1);
    g_l.custom_attach = r.prop(L"CustomAttachBone", reflect::Kind::Name);
    g_l.follow_attach = r.prop(L"bFollowAttachBoneRotation", reflect::Kind::Bool);
    g_l.ffa = r.prop(L"bFireAndForget", reflect::Kind::Bool);
    g_l.ok = r.ok() && pr.ok();
    if (!g_l.ok) log::warn("ability: AbilityDescription layout: {} / {}", r.failure(), pr.failure());
    return g_l;
}

// FTransform (float build): FQuat Rotation, FVector Translation (+pad), FVector Scale3D (+pad)
void read_xf(const std::uint8_t* t, std::array<float, 10>& xf) {
    std::memcpy(&xf[0], t, 16);
    std::memcpy(&xf[4], t + 16, 12);
    std::memcpy(&xf[7], t + 32, 12);
}
void write_xf(std::uint8_t* t, const std::array<float, 10>& xf) {
    std::memset(t, 0, 48);
    std::memcpy(t, &xf[0], 16);
    std::memcpy(t + 16, &xf[4], 12);
    std::memcpy(t + 32, &xf[7], 12);
}

using LaunchFn = UObject*(__fastcall*)(UObject*, const std::uint8_t*, const std::uint8_t*);
LaunchFn o_launch{};
using NativeLaunchFn = void(__fastcall*)(UObject*, UObject*);
using NativeTickFn = void(__fastcall*)(UObject*, float);
NativeLaunchFn o_native_launch{};
NativeTickFn o_native_tick{};
void (*g_sink)(const Launch&) = nullptr;
bool g_installed = false, g_failed = false;
std::uint64_t g_captured = 0, g_replayed = 0, g_rejected = 0;
std::uint64_t g_order = 0, g_descendants = 0, g_overflow = 0, g_expired = 0, g_reset_stops = 0, g_reset_launches = 0;
std::uint64_t g_stop_deferred = 0, g_completion_failed = 0;
std::string g_fault;
void latch_fault(const char* why) { if (g_fault.empty()) g_fault = why; }
constexpr std::size_t kActivationCapacity = 4096;
constexpr std::size_t kFamilyCapacity = 1024, kMemberCapacity = 8192, kSourceCapacity = 8192;
struct Activation {
    ObjHandle actor, invoker;
    std::uint64_t token = 0, root = 0;
    bool client = false;
    net::proto::AbilityPath path;
    std::string cls;
    bool supported = false, active = true, stop_pending = false;
};
std::unordered_map<UObject*, Activation> g_activations;
struct Member { ObjHandle actor; Identity id; };
struct SourceCounter { net::proto::AbilityPath source; std::uint32_t count = 0; };
struct Family {
    bool client = false;
    std::vector<Member> members;
    std::vector<SourceCounter> sources;
};
std::unordered_map<std::uint64_t, Family> g_families;
std::size_t g_member_count = 0, g_source_count = 0;
std::uint64_t g_paths = 0, g_unsupported = 0, g_ended = 0, g_family_retired = 0, g_lookup_conflicts = 0;
std::string g_path_failure;
UObject* g_executing = nullptr;
std::uint64_t g_executing_token = 0;
bool g_resetting = false;

Activation* entry(UObject* actor) {
    const auto it = g_activations.find(actor);
    if (it == g_activations.end()) return nullptr;
    if (it->second.actor.get() != actor) { g_activations.erase(it); ++g_expired; return nullptr; }
    return &it->second;
}
Activation* executing() {
    auto* e = entry(g_executing);
    return e && e->token == g_executing_token ? e : nullptr;
}
Identity copied(const Activation& a) { return {a.token, a.root, a.path, a.cls, a.supported, a.active}; }
bool member_active(Member& m) {
    if (!m.id.active) return false;
    auto* a = m.actor.get();
    const auto* current = a ? entry(a) : nullptr;
    if (!current || current->token != m.id.token || !current->active) m.id.active = false;
    return m.id.active;
}
bool family_active(std::uint64_t root) {
    if (const auto f = g_families.find(root); f != g_families.end())
        for (auto& m : f->second.members) if (member_active(m)) return true;
    // Capacity failures retain their exact native identity for cleanup / explicit unsupported report.
    for (const auto& [_, a] : g_activations) if (a.root == root && a.active && a.actor.valid()) return true;
    return false;
}
void prune_families() {
    const auto* e = executing();
    const auto held_root = e ? e->root : 0;
    std::erase_if(g_families, [&](const auto& p) {
        if (p.first == held_root || family_active(p.first)) return false;
        g_member_count -= p.second.members.size(); g_source_count -= p.second.sources.size();
        ++g_family_retired;
        return true;
    });
}
void clear_families(bool client) {
    std::erase_if(g_families, [client](const auto& p) {
        if (p.second.client != client) return false;
        g_member_count -= p.second.members.size(); g_source_count -= p.second.sources.size();
        return true;
    });
}
const std::string& class_path(UObject* cls) {
    static std::unordered_map<UObject*, std::pair<ObjHandle, std::string>> paths;
    auto& p = paths[cls];
    if (p.first.get() != cls) p = {ObjHandle::of(cls), log::utf8(cls->GetPathName())};
    return p.second;
}

struct ContainerSchema {
    ObjHandle cls, timeline_type, entry_type, description_type;
    reflect::PropRef timeline, entries, description;
    std::int32_t stride = 0;
    bool ready = false;
} g_container;
bool container_schema() {
    if (g_container.ready && g_container.cls.valid() && g_container.timeline_type.valid() &&
        g_container.entry_type.valid() && g_container.description_type.valid()) return true;
    g_container = {};
    auto* cls = reflect::find_class(L"/Script/Zenith.AbilityContainer");
    reflect::Resolver c("AbilityContainer", cls);
    g_container.timeline = c.prop(L"AbilityTimeline", reflect::Kind::Struct);
    if (!c.ok()) { g_path_failure = c.failure(); return false; }
    auto* ts = static_cast<ue::UStruct*>(static_cast<RC::Unreal::FStructProperty*>(g_container.timeline.prop)->GetStruct().Get());
    reflect::Resolver t("AbilityTimeline", ts);
    g_container.entries = t.prop(L"Entries", reflect::Kind::Array, sizeof(reflect::RawArray));
    auto* inner = g_container.entries.prop ? static_cast<RC::Unreal::FArrayProperty*>(g_container.entries.prop)->GetInner() : nullptr;
    if (!t.ok() || !inner || reflect::kind_of(inner) != reflect::Kind::Struct) { g_path_failure = "Container timeline Entries schema"; return false; }
    auto* es = static_cast<ue::UStruct*>(static_cast<RC::Unreal::FStructProperty*>(inner)->GetStruct().Get());
    reflect::Resolver e("AbilityTimelineEntry", es);
    g_container.description = e.prop(L"AbilityDescription", reflect::Kind::Struct);
    if (!e.ok()) { g_path_failure = e.failure(); return false; }
    auto* ds = static_cast<ue::UStruct*>(static_cast<RC::Unreal::FStructProperty*>(g_container.description.prop)->GetStruct().Get());
    auto* expected = reflect::find_object(L"/Script/Zenith.AbilityDescription");
    g_container.stride = inner->GetSize();
    if (ds != expected || g_container.stride <= 0 || g_container.stride > 4096 ||
        g_container.description.offset + g_container.description.size > g_container.stride ||
        g_container.entries.offset + g_container.entries.size > g_container.timeline.size) {
        g_path_failure = "Container entry stride/description type"; return false;
    }
    g_container.cls = ObjHandle::of(cls); g_container.timeline_type = ObjHandle::of(ts);
    g_container.entry_type = ObjHandle::of(es); g_container.description_type = ObjHandle::of(ds);
    return g_container.ready = true;
}
bool source_path(const Activation& parent, const std::uint8_t* desc, net::proto::AbilityPath& path) {
    if (!parent.supported || parent.path.depth >= net::proto::kAbilityPathDepth) { g_path_failure = "unsupported parent/depth"; return false; }
    auto* actor = parent.actor.get();
    if (!actor || !container_schema() || !actor->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(g_container.cls.get()))) {
        g_path_failure = "description parent is not a verified Container"; return false;
    }
    const auto arr = g_container.entries.read<reflect::RawArray>(g_container.timeline.at<std::uint8_t>(actor));
    if (arr.num <= 0 || arr.max < arr.num || arr.max > 65536 || !arr.data) { g_path_failure = "Container Entries bounds"; return false; }
    const auto at = reinterpret_cast<std::uintptr_t>(desc);
    const auto first = reinterpret_cast<std::uintptr_t>(arr.data) + g_container.description.offset;
    if (at < first) { g_path_failure = "description outside Container Entries"; return false; }
    const auto delta = at - first;
    if (delta % g_container.stride || delta / g_container.stride >= static_cast<std::uint32_t>(arr.num)) {
        g_path_failure = "description is not an exact Container entry"; return false;
    }
    path = parent.path;
    path.entries[path.depth] = static_cast<std::uint16_t>(delta / g_container.stride);
    path.occurrences[path.depth++] = 0; // assigned only at a successful native ready→Launch entry
    return true;
}
bool assign_occurrence(std::uint64_t root, net::proto::AbilityPath& path) {
    auto f = g_families.find(root);
    if (f == g_families.end() || !path.depth) { g_path_failure = "missing native family"; return false; }
    auto& sources = f->second.sources;
    auto it = std::find_if(sources.begin(), sources.end(), [&](const auto& s) { return s.source == path; });
    if (it == sources.end()) {
        if (g_source_count >= kSourceCapacity) { ++g_overflow; g_path_failure = "source counter capacity"; latch_fault("ability source counter capacity"); return false; }
        sources.push_back({path, 0}); ++g_source_count; it = std::prev(sources.end());
    }
    if (it->count == std::numeric_limits<std::uint32_t>::max()) { ++g_overflow; g_path_failure = "source occurrence exhausted"; latch_fault("ability source occurrence exhausted"); return false; }
    path.occurrences[path.depth - 1] = ++it->count;
    return true;
}
bool original_active(UObject* actor) {
    static reflect::FuncRef is_active = reflect::Resolver("ReusableActor", reflect::find_class(L"/Script/Zenith.ReusableActor")).func(L"IsActive", 0);
    reflect::Frame f(is_active);
    // Failed reflection cannot justify evicting a possibly running activation.
    return !is_active.ok() || !f.invoke(actor) || f.ret_bool();
}
bool completed(UObject* actor, bool& out) {
    static reflect::FuncRef is_active = reflect::Resolver("ReusableActor", reflect::find_class(L"/Script/Zenith.ReusableActor")).func(L"IsActive", 0);
    static reflect::FuncRef is_launched = reflect::Resolver("Ability", reflect::find_class(L"/Script/Zenith.Ability")).func(L"IsLaunched", 0);
    reflect::Frame a(is_active), l(is_launched);
    if (!is_active.ok() || !is_launched.ok() || !a.invoke(actor) || !l.invoke(actor)) {
        ++g_completion_failed;
        latch_fault("ability native completion state unavailable");
        return false;
    }
    // IsLaunched CAC120 only recognizes states 2/3. State 4 is still Active while its effects drain.
    out = !a.ret_bool() && !l.ret_bool();
    return true;
}
bool activation_capacity(UObject* actor) {
    if (g_activations.contains(actor)) return true;
    if (g_activations.size() < kActivationCapacity) return true;
    std::erase_if(g_activations, [](const auto& p) {
        const auto* a = p.second.actor.get();
        return !a || (p.second.token != g_executing_token && !original_active(const_cast<UObject*>(a)));
    });
    if (g_activations.size() < kActivationCapacity) return true;
    ++g_overflow;
    latch_fault("ability activation capacity");
    return false;
}

// The description scope identifies exactly one native root entry, before synchronous OnLaunch.
// Descendants inherit the executing activation, even though their AbilityComponent belongs to
// the same Invoker Pawn (AbilityContainer CB25C0 / CB2AE0), not to the container Actor.
struct DescriptionScope {
    UObject* owner = nullptr;
    UObject* cls = nullptr;
    ObjHandle spawned;
    std::uint64_t token = 0;
    bool client = false;
    bool child = false, supported = false;
    std::uint64_t parent = 0;
    net::proto::AbilityPath source;
    DescriptionScope* previous = nullptr;
    DescriptionScope(UObject* o, UObject* c, bool cl);
    ~DescriptionScope();
};
DescriptionScope* g_description = nullptr;
DescriptionScope::DescriptionScope(UObject* o, UObject* c, bool cl) : owner(o), cls(c), client(cl), previous(g_description) { g_description = this; }
DescriptionScope::~DescriptionScope() { g_description = previous; }

void __fastcall d_native_launch(UObject* actor, UObject* invoker) {
    if (!ue::on_game_thread() || !ue::alive(actor)) { o_native_launch(actor, invoker); return; }
    if (!g_sink && !g_description && g_activations.empty()) { o_native_launch(actor, invoker); return; }
    const auto* parent = executing();
    // Stop cleanup may emit tail particles, but cannot leave a new live ability after reset's
    // ownership snapshot is discarded. Description callers are rejected before pool allocation too.
    if (g_resetting && parent && parent->client) { ++g_reset_launches; return; }
    // A rejected duplicate Launch must not revoke the activation vanilla keeps running.
    if (original_active(actor)) { o_native_launch(actor, invoker); return; }
    const bool description_matches = g_description && !g_description->token && g_description->owner == invoker && g_description->cls == actor->GetClassPrivate();
    const bool root = description_matches && !g_description->child;
    const bool child = parent && parent->invoker.get() == invoker && invoker;
    if (!root && !child) {
        // A new unrelated use of this pooled UObject revokes the former permission.
        g_activations.erase(actor);
        o_native_launch(actor, invoker);
        return;
    }
    // Original Launch accepts only reusable ready state. Do not replace the identity of an
    // already active actor if someone calls Launch again and vanilla rejects it.
    if (!activation_capacity(actor)) { o_native_launch(actor, invoker); return; }
    const auto token = next_capture_order();
    if (!token) { ++g_overflow; latch_fault("ability native ordinal exhausted"); o_native_launch(actor, invoker); return; }
    const auto root_token = root ? token : parent->root;
    const bool client = root ? g_description->client : parent->client;
    Activation a{ObjHandle::of(actor), ObjHandle::of(invoker), token, root_token, client};
    a.cls = class_path(actor->GetClassPrivate());
    if (root) {
        prune_families();
        if (g_families.size() >= kFamilyCapacity || g_member_count >= kMemberCapacity) {
            ++g_overflow; latch_fault("ability family/member capacity"); o_native_launch(actor, invoker); return;
        }
        g_families.emplace(token, Family{client});
        a.supported = true;
    } else {
        ++g_descendants;
        if (description_matches && g_description->child && g_description->parent == parent->token && g_description->supported) {
            a.path = g_description->source;
            a.supported = assign_occurrence(root_token, a.path);
        } else g_path_failure = "native descendant has no exact Container source";
        if (g_member_count >= kMemberCapacity) { ++g_overflow; a.supported = false; g_path_failure = "family member capacity"; latch_fault("ability family member capacity"); }
        if (a.supported) ++g_paths; else ++g_unsupported;
    }
    g_activations[actor] = a;
    if (auto f = g_families.find(root_token); f != g_families.end() && g_member_count < kMemberCapacity) {
        f->second.members.push_back({a.actor, copied(a)}); ++g_member_count;
    }
    if (description_matches) { g_description->spawned = ObjHandle::of(actor); g_description->token = token; }
    NativeScope scope(actor);
    o_native_launch(actor, invoker);
    // OnLaunch can request Stop while native state is 2; CAC480 may complete it inline without
    // another Stop entry. Reconcile only that pending exact token, never a reentrant replacement.
    if (const auto* current = entry(actor); current && current->token == token && current->stop_pending) stopped(actor, copied(*current));
}

void __fastcall d_native_tick(UObject* actor, float delta) {
    if (!ue::on_game_thread()) { o_native_tick(actor, delta); return; }
    NativeScope scope(actor);
    o_native_tick(actor, delta);
    if (const auto* current = entry(actor); current && current->token == g_executing_token && current->stop_pending) stopped(actor, copied(*current));
}

UObject* __fastcall d_launch(UObject* comp, const std::uint8_t* desc, const std::uint8_t* offset) {
    // The original root already owns these launches, including later native container ticks.
    if (ue::on_game_thread()) if (const auto* parent = executing()) {
        if (g_resetting && parent->client) { ++g_reset_launches; return nullptr; }
        if (comp && parent->invoker.get() == comp->GetOuterPrivate()) {
            if (!desc || !layout().ok) return o_launch(comp, desc, offset);
            DescriptionScope scope(comp->GetOuterPrivate(), *g_l.cls.at<UObject*>(desc), parent->client);
            scope.child = true; scope.parent = parent->token;
            scope.supported = source_path(*parent, desc, scope.source);
            return o_launch(comp, desc, offset);
        }
    }
    if (g_sink && desc && offset && ue::on_game_thread() && layout().ok) {
        auto* cls = *g_l.cls.at<UObject*>(desc);
        if (ue::alive(cls)) {
            // fields are read before the call (v1 net-facts: the description must be read pre-call); the
            // sink runs after it with the launched ability (ActStop mapping)
            Launch l;
            l.owner = comp ? comp->GetOuterPrivate() : nullptr;
            // path per class cached (GetPathName walks outers + builds strings); entry valid while the
            // weak handle still resolves to the same class object
            l.cls = class_path(cls);
            const auto* params = g_l.params.at<std::uint8_t>(desc);
            l.act.max_execution_time = *g_l.max_time.at<float>(params);
            l.act.damage_rate = *g_l.damage_rate.at<float>(params);
            read_xf(g_l.offset.at<std::uint8_t>(desc), l.act.xf);
            read_xf(offset, l.act.xf2);
            l.act.spawn_bone = *g_l.spawn_bone.at<std::uint8_t>(desc);
            l.act.attach_bone = *g_l.attach_bone.at<std::uint8_t>(desc);
            l.custom_spawn = ue::utf8(*g_l.custom_spawn.at<ue::FName>(desc));
            l.custom_attach = ue::utf8(*g_l.custom_attach.at<ue::FName>(desc));
            l.act.flags = (g_l.follow_spawn.read_bool(desc) ? 1 : 0) | (g_l.follow_attach.read_bool(desc) ? 2 : 0) | (g_l.ffa.read_bool(desc) ? 4 : 0);
            DescriptionScope scope(l.owner, cls, false);
            l.spawned = o_launch(comp, desc, offset);
            l.activation = scope.token;
            if (!l.activation || scope.spawned.get() != l.spawned) { ++g_rejected; return l.spawned; }
            ++g_captured;
            if (g_sink) g_sink(l);
            return l.spawned;
        }
    }
    return o_launch(comp, desc, offset);
}

} // namespace

std::uint64_t next_capture_order() {
    if (g_order != std::numeric_limits<std::uint64_t>::max()) return ++g_order;
    if (g_sink || !g_activations.empty()) latch_fault("ability capture order exhausted");
    return 0;
}
std::string fault() { return g_fault; }
void reset_fault() { g_fault.clear(); }
std::uint64_t activation(UObject* actor) { const auto* e = entry(actor); return e ? e->token : 0; }
bool native_writer(UObject* actor) { return activation(actor) != 0; }
bool identity(UObject* actor, Identity& out) {
    out = {};
    const auto* a = entry(actor);
    if (!a) return false;
    out = copied(*a);
    return true;
}
namespace {
ChildState lookup_child(std::uint64_t root, const net::proto::AbilityPath& path, const std::string& cls, Member*& found) {
    found = nullptr;
    if (!root || !path.depth || path.depth > net::proto::kAbilityPathDepth || cls.empty()) return ChildState::Conflict;
    for (std::uint8_t i = 0; i < path.depth; ++i) if (!path.occurrences[i]) return ChildState::Conflict;
    const auto f = g_families.find(root);
    if (f == g_families.end()) return ChildState::Missing;
    for (auto& m : f->second.members) if (m.id.supported && m.id.path == path) {
        if (found || m.id.cls != cls) { ++g_lookup_conflicts; return ChildState::Conflict; }
        found = &m;
    }
    if (!found) return ChildState::Missing;
    return member_active(*found) ? ChildState::Active : ChildState::Ended;
}
}
ChildState child_state(std::uint64_t root, const net::proto::AbilityPath& path, const std::string& cls) {
    Member* found = nullptr;
    return lookup_child(root, path, cls, found);
}
UObject* find_child(std::uint64_t root, const net::proto::AbilityPath& path, const std::string& cls, std::uint64_t* token_out) {
    if (token_out) *token_out = 0;
    Member* found = nullptr;
    if (lookup_child(root, path, cls, found) != ChildState::Active) return nullptr;
    if (token_out) *token_out = found->id.token;
    return found->actor.get();
}
bool has_active_family(std::uint64_t root) { return root && family_active(root); }
void stopped(UObject* actor, const Identity& before) {
    if (!before.token) return;
    if (auto* current = entry(actor); current && current->token == before.token) {
        bool done = false;
        if (!completed(actor, done) || !done) {
            if (!current->stop_pending) ++g_stop_deferred;
            current->stop_pending = true;
            return;
        }
        current->active = false;
        current->stop_pending = false;
    }
    if (const auto f = g_families.find(before.root); f != g_families.end())
        for (auto& m : f->second.members) if (m.id.token == before.token && m.id.active) { m.id.active = false; ++g_ended; break; }
}
NativeScope::NativeScope(UObject* actor) : previous_actor_(g_executing), previous_token_(g_executing_token) {
    g_executing = actor;
    g_executing_token = activation(actor);
}
NativeScope::~NativeScope() { g_executing = previous_actor_; g_executing_token = previous_token_; }

bool install_hooks() {
    if (!g_installed && !g_failed) {
        const bool a = hook::install_manifest("AbilityLaunchNative", reinterpret_cast<void*>(&d_native_launch), reinterpret_cast<void**>(&o_native_launch));
        const bool t = hook::install_manifest("AbilityTickNative", reinterpret_cast<void*>(&d_native_tick), reinterpret_cast<void**>(&o_native_tick));
        const bool d = hook::install_manifest("AbilityLaunchFromDesc", reinterpret_cast<void*>(&d_launch), reinterpret_cast<void**>(&o_launch));
        g_installed = a && t && d;
        g_failed = !g_installed;
    }
    return g_installed;
}

bool host_enable(void (*sink)(const Launch&)) {
    if (!g_sink && sink) reset_fault(); // tick reasserts the sink; only a new session resets failures
    install_hooks();
    g_sink = g_installed ? sink : nullptr;
    return g_installed;
}

void host_disable() {
    g_sink = nullptr;
    std::erase_if(g_activations, [](const auto& p) { return !p.second.client; });
    clear_families(false);
    reset_fault();
}

bool replay(ue::UObject* enemy, const net::proto::Act& act, const std::string& cls_path, const std::string& custom_spawn, const std::string& custom_attach,
            std::string* why, ue::UObject** spawned_out, std::uint64_t* activation_out) {
    if (spawned_out) *spawned_out = nullptr;
    if (activation_out) *activation_out = 0;
    auto fail = [&](const char* w) {
        ++g_rejected;
        if (why) *why = w;
        return false;
    };
    if (!install_hooks()) return fail("native activation hooks");
    if (!layout().ok) return fail("layout");
    static ue::UClass* ability_base = reflect::find_class(L"/Script/Zenith.Ability");
    // class lookup by path costs milliseconds (StaticLoadObject string resolve) → cache per path, keyed by
    // a weak handle (invalidated when the class is unloaded)
    static std::unordered_map<std::string, ObjHandle> cache;
    auto& slot = cache[cls_path];
    auto* cls = static_cast<ue::UClass*>(slot.get());
    if (!cls) {
        cls = static_cast<ue::UClass*>(reflect::load_object(log::wide(cls_path)));
        slot = ObjHandle::of(cls);
    }
    if (!cls || !ability_base || !cls->IsChildOf(ability_base)) return fail("not an Ability class");
    static ObjHandle comp_cls = ObjHandle::of(reflect::find_object(L"/Script/Zenith.AbilityComponent"));
    static reflect::FuncRef by_class, launch;
    if (!launch.ok()) {
        by_class = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"GetComponentByClass", 1);
        launch = reflect::Resolver("AbilityComponent", reflect::find_class(L"/Script/Zenith.AbilityComponent")).func(L"LaunchAbilityFromDescription", 2);
    }
    if (!launch.ok() || !by_class.ok()) return fail("LaunchAbilityFromDescription");
    reflect::Frame c(by_class);
    c.arg(0, comp_cls.get());
    auto* comp = c.invoke(enemy) ? c.ret<ue::UObject*>() : nullptr;
    if (!ue::alive(comp)) return fail("no AbilityComponent");
    reflect::Frame f(launch);
    auto* desc = static_cast<std::uint8_t*>(f.raw(0));
    auto* off = static_cast<std::uint8_t*>(f.raw(1));
    if (!desc || !off) return fail("frame");
    *g_l.cls.at<ue::UClass*>(desc) = cls;
    auto* params = g_l.params.at<std::uint8_t>(desc);
    *g_l.max_time.at<float>(params) = act.max_execution_time;
    *g_l.damage_rate.at<float>(params) = act.damage_rate;
    write_xf(g_l.offset.at<std::uint8_t>(desc), act.xf);
    write_xf(off, act.xf2);
    *g_l.spawn_bone.at<std::uint8_t>(desc) = act.spawn_bone;
    *g_l.attach_bone.at<std::uint8_t>(desc) = act.attach_bone;
    *g_l.custom_spawn.at<ue::FName>(desc) = custom_spawn.empty() ? ue::FName() : ue::FName(log::wide(custom_spawn).c_str(), RC::Unreal::FNAME_Add);
    *g_l.custom_attach.at<ue::FName>(desc) = custom_attach.empty() ? ue::FName() : ue::FName(log::wide(custom_attach).c_str(), RC::Unreal::FNAME_Add);
    g_l.follow_spawn.write_bool(desc, act.flags & 1);
    g_l.follow_attach.write_bool(desc, act.flags & 2);
    g_l.ffa.write_bool(desc, act.flags & 4);
    DescriptionScope scope(enemy, cls, true);
    if (!f.invoke(comp)) return fail("invoke");
    auto* spawned = scope.spawned.get();
    if (!scope.token || !spawned || f.ret<UObject*>() != spawned) return fail("native root activation");
    if (spawned_out) *spawned_out = spawned;
    if (activation_out) *activation_out = scope.token;
    ++g_replayed;
    return true;
}

void client_reset() {
    struct Bound { ObjHandle actor; std::uint64_t token; };
    std::vector<Bound> live;
    for (const auto& [_, e] : g_activations) if (e.client) live.push_back({e.actor, e.token});
    static reflect::FuncRef stop = reflect::Resolver("Ability", reflect::find_class(L"/Script/Zenith.Ability")).func(L"Stop", 1);
    static reflect::FuncRef active = reflect::Resolver("ReusableActor", reflect::find_class(L"/Script/Zenith.ReusableActor")).func(L"IsActive", 0);
    struct ResetScope { bool previous = g_resetting; ResetScope() { g_resetting = true; } ~ResetScope() { g_resetting = previous; } } reset_scope;
    for (const auto& b : live) if (auto* actor = b.actor.get(); actor && activation(actor) == b.token) {
        reflect::Frame q(active);
        if (stop.ok() && q.invoke(actor) && q.ret_bool()) {
            NativeScope scope(actor);
            reflect::Frame f(stop);
            f.arg(0, std::uint8_t{3}); // END_PLAY: native Stop completes even with queued effects.
            if (f.invoke(actor)) ++g_reset_stops;
        }
    }
    std::erase_if(g_activations, [](const auto& p) { return p.second.client; });
    clear_families(true);
}

std::string describe() {
    return std::format("ability: hook={} captured={} replayed={} rejected={} activations={} descendants={} identityOverflow={} expired={} resetStops={} resetLaunchRejected={}\n"
                       "  native families={} members={} sources={} paths={} unsupported={} ended={} retiredFamilies={} lookupConflicts={} deferredStops={} completionFailed={} pathLast='{}' fault='{}'\n",
                       g_installed ? "armed" : (g_failed ? "failed" : "off"), g_captured, g_replayed, g_rejected, g_activations.size(), g_descendants, g_overflow,
                       g_expired, g_reset_stops, g_reset_launches, g_families.size(), g_member_count, g_source_count, g_paths, g_unsupported, g_ended, g_family_retired, g_lookup_conflicts, g_stop_deferred, g_completion_failed, g_path_failure, g_fault);
}

} // namespace elmp::coop::ability
