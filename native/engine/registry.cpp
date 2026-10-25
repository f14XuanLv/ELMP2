#include "engine/registry.h"

#include "base/clock.h"
#include "base/log.h"
#include "base/metrics.h"

#include <Unreal/AActor.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace elmp::registry {

using namespace RC::Unreal;

namespace {

struct KindDef {
    Kind kind;
    const wchar_t* cls;
    const wchar_t* package; // nullptr = blueprint class, name is unique enough ("..._C")
};

// Class names: Zenith reflected headers (D:\CodeT\EnderLilies\Source\Zenith\Public) and v1 kb.
constexpr KindDef kDefs[] = {
    {Kind::Enemy, L"BP_Character_Enemy_Base_C", nullptr},
    {Kind::SpawnPoint, L"EnemySpawnPoint", L"/Script/Zenith"},
    {Kind::Lily, L"BP_Character_Player_Base_C", nullptr},
    {Kind::PlayerController, L"ZenithPlayerController", L"/Script/Zenith"},
    {Kind::Interactable, L"Interactable", L"/Script/Zenith"},
    {Kind::RestingPoint, L"BP_Interactable_RestingPoint_C", nullptr},
    {Kind::Breakable, L"Breakable", L"/Script/Zenith"},
    {Kind::Collectable, L"Collectable", L"/Script/Zenith"},
    {Kind::Trigger, L"Trigger", L"/Script/Zenith"},
    {Kind::TravelVolume, L"BP_WorldTravelVolume_C", nullptr},
    {Kind::ActivationVolume, L"PawnActivationVolume", L"/Script/Zenith"},
    {Kind::Menu, L"UserWidgetNavigable", L"/Script/Zenith"},
};
constexpr std::uint32_t kMenuBit = 1u << static_cast<int>(Kind::Menu);

struct DefKeys {
    std::uint32_t name = 0, package = 0;
};
std::array<DefKeys, std::size(kDefs)> g_keys;

struct ClassInfo {
    std::uint32_t mask = 0;
    std::uint16_t id = 0;
};

struct Member {
    std::int32_t serial = 0;
    std::uint32_t mask = 0;
};

struct State {
    std::array<std::vector<Entry>, kKinds> buckets;
    std::array<std::unordered_map<std::int32_t, std::uint32_t>, kKinds> pos; // object index → bucket position
    std::unordered_map<std::int32_t, Member> members;                          // object index → membership
    std::unordered_map<ObjHandle, ClassInfo> classes;
    std::vector<std::string> class_names{""};
    Stats stats;
    std::vector<Hook::GlobalCallbackId> hooks;
};

State g;
std::atomic<bool> g_menu{false};
std::mutex g_pending_mutex;
std::vector<std::int32_t> g_pending_created, g_pending_deleted;
bool g_installed = false;

metrics::Counter& c_stale = metrics::counter("registry.audit.stale");

std::uint32_t key(const wchar_t* text) { return ue::name_key(FName(text, FNAME_Add)); }

ClassInfo& class_info(UClass* cls) {
    const auto h = ObjHandle::of(cls);
    auto it = g.classes.find(h);
    if (it != g.classes.end()) return it->second;
    ClassInfo info;
    for (UStruct* s = cls; s; s = s->GetSuperStruct()) {
        const auto n = ue::name_key(s->GetNamePrivate());
        for (std::size_t i = 0; i < std::size(kDefs); ++i) {
            if (g_keys[i].name != n) continue;
            if (g_keys[i].package) {
                UObject* outer = s->GetOuterPrivate();
                if (!outer || ue::name_key(outer->GetNamePrivate()) != g_keys[i].package) continue;
            }
            info.mask |= 1u << static_cast<int>(kDefs[i].kind);
        }
    }
    if (info.mask) {
        info.id = static_cast<std::uint16_t>(g.class_names.size());
        g.class_names.push_back(log::utf8(cls->GetName()));
    }
    return g.classes.emplace(h, info).first->second;
}

void insert(UObject* o, std::uint32_t mask, std::uint16_t class_id) {
    const auto h = ObjHandle::of(o);
    auto& m = g.members[h.index];
    if (m.serial != h.serial) {
        // Index reused by a new object: drop the old membership first.
        for (std::size_t k = 0; k < kKinds; ++k) {
            if (!(m.mask & (1u << k))) continue;
            auto& p = g.pos[k];
            auto it = p.find(h.index);
            if (it == p.end()) continue;
            auto& b = g.buckets[k];
            const auto at = it->second;
            p.erase(it);
            if (at + 1 != b.size()) {
                b[at] = b.back();
                p[b[at].handle.index] = at;
            }
            b.pop_back();
        }
        m = Member{h.serial, 0};
    }
    const std::uint32_t add = mask & ~m.mask;
    if (!add) return;
    m.mask |= add;
    for (std::size_t k = 0; k < kKinds; ++k) {
        if (!(add & (1u << k))) continue;
        g.pos[k][h.index] = static_cast<std::uint32_t>(g.buckets[k].size());
        g.buckets[k].push_back(Entry{h, o->GetNamePrivate(), class_id});
    }
    ++g.stats.inserted;
}

bool remove_index(std::int32_t index, std::int32_t serial_or_zero) {
    auto it = g.members.find(index);
    if (it == g.members.end()) return false;
    if (serial_or_zero && it->second.serial != serial_or_zero) return false;
    for (std::size_t k = 0; k < kKinds; ++k) {
        if (!(it->second.mask & (1u << k))) continue;
        auto& p = g.pos[k];
        auto pit = p.find(index);
        if (pit == p.end()) continue;
        auto& b = g.buckets[k];
        const auto at = pit->second;
        p.erase(pit);
        if (at + 1 != b.size()) {
            b[at] = b.back();
            p[b[at].handle.index] = at;
        }
        b.pop_back();
    }
    g.members.erase(it);
    return true;
}

void consider(UObject* o) {
    if (!o || o->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject))) return;
    UClass* cls = o->GetClassPrivate();
    if (!cls) return;
    const auto& info = class_info(cls);
    std::uint32_t mask = info.mask;
    if (!g_menu.load(std::memory_order_relaxed)) mask &= ~kMenuBit;
    if (mask && ObjHandle::of(o).get()) insert(o, mask, info.id);
}

class Listeners final : public FUObjectCreateListener, public FUObjectDeleteListener {
public:
    void NotifyUObjectCreated(const UObjectBase*, int32 index) override {
        std::scoped_lock lock(g_pending_mutex);
        g_pending_created.push_back(index);
    }
    void NotifyUObjectDeleted(const UObjectBase*, int32 index) override {
        if (ue::on_game_thread()) {
            if (remove_index(index, 0)) ++g.stats.removed_deleted;
            return;
        }
        std::scoped_lock lock(g_pending_mutex);
        g_pending_deleted.push_back(index);
    }
    void OnUObjectArrayShutdown() override {}
};
Listeners g_listeners;

template <class F>
void walk_objects(F&& visit) {
    UObjectGlobals::ForEachUObject([&](UObject* o, int32, int32) {
        if (o) visit(o);
        return RC::LoopAction::Continue;
    });
}

} // namespace

const char* kind_name(Kind k) noexcept {
    switch (k) {
    case Kind::Enemy: return "enemy";
    case Kind::SpawnPoint: return "spawnPoint";
    case Kind::Lily: return "lily";
    case Kind::PlayerController: return "playerController";
    case Kind::Interactable: return "interactable";
    case Kind::RestingPoint: return "restingPoint";
    case Kind::Breakable: return "breakable";
    case Kind::Collectable: return "collectable";
    case Kind::Trigger: return "trigger";
    case Kind::TravelVolume: return "travelVolume";
    case Kind::ActivationVolume: return "activationVolume";
    case Kind::Menu: return "menu";
    case Kind::Count: break;
    }
    return "?";
}

void install() {
    if (g_installed) return;
    g_installed = true;
    for (std::size_t i = 0; i < std::size(kDefs); ++i) {
        g_keys[i].name = key(kDefs[i].cls);
        g_keys[i].package = kDefs[i].package ? key(kDefs[i].package) : 0;
    }
    Hook::FCallbackOptions opt;
    opt.OwnerModName = STR("ELMP");
    opt.HookName = STR("registry.beginPlay");
    g.hooks.push_back(Hook::RegisterBeginPlayPostCallback([](auto&, AActor* actor) { consider(reinterpret_cast<UObject*>(actor)); }, opt));
    opt.HookName = STR("registry.endPlay");
    g.hooks.push_back(Hook::RegisterEndPlayPreCallback(
        [](auto&, AActor* actor, EEndPlayReason) {
            const auto h = ObjHandle::of(reinterpret_cast<UObject*>(actor));
            if (remove_index(h.index, h.serial)) ++g.stats.removed_endplay;
        },
        opt));
    FUObjectArray::AddUObjectDeleteListener(&g_listeners);
}

void uninstall() {
    if (!g_installed) return;
    want_menu(false);
    FUObjectArray::RemoveUObjectDeleteListener(&g_listeners);
    for (auto id : g.hooks) Hook::UnregisterCallback(id);
    g.hooks.clear();
    g_installed = false;
}

void want_menu(bool on) {
    if (g_menu.exchange(on) == on) return;
    if (on) {
        FUObjectArray::AddUObjectCreateListener(&g_listeners);
        walk_objects([](UObject* o) {
            if (!o->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) && (class_info(o->GetClassPrivate()).mask & kMenuBit))
                insert(o, kMenuBit, class_info(o->GetClassPrivate()).id);
        });
    } else {
        FUObjectArray::RemoveUObjectCreateListener(&g_listeners);
        auto entries = g.buckets[static_cast<int>(Kind::Menu)];
        for (const auto& e : entries) {
            auto it = g.members.find(e.handle.index);
            if (it == g.members.end()) continue;
            if ((it->second.mask &= ~kMenuBit) == 0) g.members.erase(it);
        }
        g.buckets[static_cast<int>(Kind::Menu)].clear();
        g.pos[static_cast<int>(Kind::Menu)].clear();
    }
}

bool menu_wanted() { return g_menu.load(); }

void drain() {
    std::vector<std::int32_t> created, deleted;
    {
        std::scoped_lock lock(g_pending_mutex);
        if (g_pending_created.empty() && g_pending_deleted.empty()) return;
        created.swap(g_pending_created);
        deleted.swap(g_pending_deleted);
    }
    for (auto index : deleted)
        if (remove_index(index, 0)) ++g.stats.removed_deleted;
    if (!g_menu.load()) return;
    for (auto index : created) {
        auto* item = FUObjectArray::IndexToObject(index);
        UObject* o = item ? item->GetUObject() : nullptr;
        if (o && ue::alive(o) && (class_info(o->GetClassPrivate()).mask & kMenuBit)) insert(o, kMenuBit, class_info(o->GetClassPrivate()).id);
    }
}

void baseline() {
    const auto t0 = clock::now_us();
    const auto before = g.stats.inserted;
    walk_objects([](UObject* o) { consider(o); });
    g.stats.baseline_added += g.stats.inserted - before;
    ++g.stats.baselines;
    g.stats.baseline_ms = static_cast<double>(clock::now_us() - t0) / 1000.0;
    log::info("registry baseline: {} new entries in {:.1f} ms", g.stats.inserted - before, g.stats.baseline_ms);
}

void clear() {
    const bool menu = g_menu.load();
    for (std::size_t k = 0; k < kKinds; ++k) {
        if (menu && k == static_cast<std::size_t>(Kind::Menu)) continue; // widgets outlive worlds; deletes remove them
        for (const auto& e : g.buckets[k]) {
            auto it = g.members.find(e.handle.index);
            if (it != g.members.end() && (it->second.mask &= ~(1u << k)) == 0) g.members.erase(it);
        }
        g.buckets[k].clear();
        g.pos[k].clear();
    }
    g.classes.clear(); // blueprint classes of the old world may be collected
}

void audit() {
    ++g.stats.audits;
    std::vector<std::int32_t> stale;
    for (const auto& b : g.buckets)
        for (const auto& e : b)
            if (!e.handle.get()) stale.push_back(e.handle.index);
    for (auto index : stale)
        if (remove_index(index, 0)) {
            ++g.stats.audit_stale;
            ++c_stale;
        }
}

std::span<const Entry> bucket(Kind k) { return g.buckets[static_cast<std::size_t>(k)]; }
std::size_t size(Kind k) { return g.buckets[static_cast<std::size_t>(k)].size(); }

std::string class_name(std::uint16_t id) { return id < g.class_names.size() ? g.class_names[id] : std::string{}; }

Stats stats() {
    Stats s = g.stats;
    for (std::size_t k = 0; k < kKinds; ++k) s.count[k] = static_cast<std::uint32_t>(g.buckets[k].size());
    return s;
}

bool VerifyResult::consistent() const {
    for (std::size_t k = 0; k < kKinds; ++k)
        if (missing[k] || extra[k]) return false;
    return true;
}

VerifyResult cross_check() {
    const auto t0 = clock::now_us();
    VerifyResult r;
    std::array<std::unordered_set<ObjHandle>, kKinds> seen;
    const bool menu = g_menu.load();
    walk_objects([&](UObject* o) {
        if (o->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) || !o->GetClassPrivate()) return;
        std::uint32_t mask = class_info(o->GetClassPrivate()).mask;
        if (!menu) mask &= ~kMenuBit;
        if (!mask) return;
        const auto h = ObjHandle::of(o);
        if (!h.get()) return;
        for (std::size_t k = 0; k < kKinds; ++k) {
            if (!(mask & (1u << k))) continue;
            seen[k].insert(h);
            ++r.scanned[k];
            const auto it = g.pos[k].find(h.index);
            if (it == g.pos[k].end() || g.buckets[k][it->second].handle != h) {
                ++r.missing[k];
                if (r.samples.size() < 16) r.samples.push_back(std::string(kind_name(static_cast<Kind>(k))) + ": " + ue::utf8_name(o) + " (missing)");
            }
        }
    });
    for (std::size_t k = 0; k < kKinds; ++k) {
        r.registered[k] = static_cast<std::uint32_t>(g.buckets[k].size());
        for (const auto& e : g.buckets[k]) {
            if (seen[k].contains(e.handle)) continue;
            ++r.extra[k];
            if (r.samples.size() < 16) r.samples.push_back(std::string(kind_name(static_cast<Kind>(k))) + ": " + ue::utf8(e.name) + " (extra)");
        }
    }
    r.ms = static_cast<double>(clock::now_us() - t0) / 1000.0;
    return r;
}

} // namespace elmp::registry
