// Coop host (§7.5 host/{capture,guest,events}): every connected peer gets a remote seat (party Source::Network)
// fed by its input stream; every frame the host captures one FrameSnapshot (lily rows: P1 + seats; enemy rows:
// registry Enemy bucket keyed by the SourceSpawnPoint name) and sends each peer a delta against the snapshot
// it acknowledged. Names travel once as NameDef (reliable). Disconnect without F_CLOSE keeps the seat parked
// under the guest key for reconnection (FR-NET-2c); an explicit leave removes it.

#include "coop/host.h"
#include "coop/native_lily.h"
#include "coop/autonomous.h"
#include "coop/lily_events.h"
#include "coop/causality.h"
#include <unordered_set>
#include "coop/presentation.h"
#include "coop/visual_state.h"
#include "coop/reaction.h"

#include "coop/ability.h"
#include "coop/fx.h"
#include "coop/entity.h"
#include "coop/entities/companion.h"
#include "coop/maps.h"
#include "coop/session.h"

#include "party/seat.h"
#include "party/transit.h"

#include "core/player.h"
#include "core/profile.h"
#include "core/schema.h"
#include "core/skin.h"

#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "input/virtual_pad.h"

#include "net/protocol.h"
#include "net/snapshot.h"

#include "base/clock.h"
#include "base/json.h"
#include "base/log.h"
#include "base/metrics.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

namespace elmp::coop::host {

namespace {

namespace snap = net::snap;
namespace proto = net::proto;

constexpr std::size_t kRing = 32;
constexpr std::int64_t kStallUs = 500'000;

struct Peer {
    std::uint32_t id = 0;
    std::int64_t slot = 0;
    std::string guest_key;
    std::uint16_t epoch = 1;
    std::uint32_t seq = 0, acked = 0;
    std::unique_ptr<std::array<snap::Frame, kRing>> ring = std::make_unique<std::array<snap::Frame, kRing>>();
    std::array<std::uint16_t, 10> presses{};
    bool presses_known = false;
    std::int64_t last_input_us = 0;
    bool stalled = true;
    std::uint16_t map_sent = 0;
    std::uint16_t map_ready = 0;
    std::string build_report;
    std::array<std::string, 9> native_builds;
    std::uint16_t native_map = 0;
    std::uint32_t native_view = 0;
    std::deque<proto::LilyFrame> native_frames;
    std::unordered_set<std::uint64_t> native_roots;
    std::unordered_map<std::uint32_t,std::int64_t> native_spirit_commands;
    std::unordered_map<std::uint64_t,std::int64_t> native_retired;
};

std::map<std::uint32_t, Peer> g_peers;
std::map<std::string, std::int64_t> g_parked; // guest key → seat kept after an unexpected disconnect
proto::NameTable g_names;
std::unique_ptr<snap::Frame> g_frame = std::make_unique<snap::Frame>();
std::array<std::uint8_t, 128 * 1024> g_buf;
std::uint16_t g_epoch = 1;
std::uint16_t g_adapter_map = 0;
std::string g_trace_name;
std::vector<float> g_trace;
reflect::PropRef g_spawn_point;
bool g_resolved = false;
std::uint64_t g_engaged_rows = 0; // P8 Dev：本帧带 bit3（战斗中）的敌人行数，describe 里看得到
metrics::Counter& c_snaps = metrics::counter("coop.host.snapshots");
metrics::Counter& c_snap_fail = metrics::counter("coop.host.snapshot_encode_failed");
metrics::Histogram& h_capture = metrics::histogram("coop.host.capture_us");
void lifecycle_fault(const char* reason);

void send_rel(std::uint32_t peer, std::size_t n) {
    if (n) coop::send_reliable(peer, std::span(g_buf.data(), n));
}

std::uint16_t name(std::string_view s) {
    bool fresh = false;
    const auto id = g_names.intern(s, &fresh);
    if (fresh)
        for (auto& [pid, p] : g_peers) send_rel(pid, proto::encode(proto::NameDef{id, std::string(s)}, g_buf));
    return id;
}

void resolve() {
    if (g_resolved) return;
    reflect::Resolver r("ZenithCharacter", reflect::find_class(L"/Script/Zenith.ZenithCharacter"));
    g_spawn_point = r.prop(L"SourceSpawnPoint", reflect::Kind::Object);
    g_resolved = r.ok();
}

// P8：敌人“在战斗中”（拿到了目标）。原版由 AI 的 OnChangeTarget → BattleColor->SetBattleColorEnabled(true)
// 打开材质参数的红光 + BP_FXComponent->TriggerFindEnemyFX()（PS_FindEnemy）+ enemy-find-enemy 音效。
// 这条状态不在任何 FX 通道里（红光只是材质参数，不是粒子），客户端又因为 AI 闸永远拿不到目标，
// 所以它必须随敌人快照过去（EnemyRow.flags bit3），由客户端调同一个原版事件在本地重现。
reflect::PropRef& enemy_target() {
    static reflect::PropRef p;
    if (!p.ok()) {
        if (auto* cls = reflect::find_class(L"/Game/_Zenith/Core/Pawns/BP_Character_Enemy_Base.BP_Character_Enemy_Base_C"))
            p = reflect::Resolver("EnemyBase", cls).prop(L"CurrentTarget", reflect::Kind::Object);
        if (!p.ok()) log::warn("coop: enemy CurrentTarget unresolved — battle colour state will not replicate");
        else log::info("coop: enemy CurrentTarget at +{}", p.offset);
    }
    return p;
}

void lily_row(std::uint16_t key, ue::UObject* pawn, const core::schema::Script& s) {
    auto* row = g_frame->l.add(key);
    if (!row || !pawn) return;
    if (auto at = core::location(pawn)) row->x = at->x, row->y = at->y, row->z = at->z;
    if (auto* hp = s.zc_hp.object(pawn)) {
        row->hp = static_cast<std::uint16_t>(std::max(0, s.hp_curr.read<std::int32_t>(hp)));
        row->mhp = static_cast<std::uint16_t>(std::max(0, s.hp_max.read<std::int32_t>(hp)));
        row->alive = row->hp > 0;
    }
    if (auto* mp = s.zc_mp.object(pawn)) {
        row->mp = static_cast<std::uint16_t>(std::max(0, s.mp_curr.read<std::int32_t>(mp)));
        row->mmp = static_cast<std::uint16_t>(std::max(0, s.mp_max.read<std::int32_t>(mp)));
    }
    if (auto* loco = s.zc_locomotion.object(pawn)) {
        reflect::Frame f(s.loco_facing_right);
        if (f.invoke(loco)) row->facing = f.ret_bool() ? 1 : -1;
    }
    row->skin = static_cast<std::uint16_t>(std::max(0, core::skin::current(pawn)));
    if(!lily_events::capture(pawn,name,row->events))lifecycle_fault("Lily external animation context capture failed");

}

} // namespace

std::uint8_t enemy_flags(ue::UObject* actor) {
    if (!ue::alive(actor)) return 0;
    static reflect::PropRef hidden;
    static reflect::FuncRef ticking;
    static ObjHandle enemy_base;
    if (!hidden.ok() || !ticking.ok()) {
        reflect::Resolver r("EnemyOutputActor", reflect::find_class(L"/Script/Engine.Actor"));
        hidden = r.prop(L"bHidden", reflect::Kind::Bool);
        ticking = r.func(L"IsActorTickEnabled", 0);
    }
    if (!enemy_base.valid()) enemy_base = ObjHandle::of(reflect::find_class(L"/Game/_Zenith/Core/Pawns/BP_Character_Enemy_Base.BP_Character_Enemy_Base_C"));
    std::uint8_t flags = hidden.ok() && hidden.read_bool(actor) ? 2 : 0;
    reflect::Frame f(ticking);
    if (f.invoke(actor) && f.ret_bool()) flags |= 1;
    if (auto* cls = static_cast<ue::UClass*>(enemy_base.get()); cls && actor->GetClassPrivate()->IsChildOf(cls))
        if (enemy_target().ok() && enemy_target().object(actor)) flags |= 8;
    return flags;
}

// adapter lily (host half): P1 + every seat
void write_lilies(HostCtx&) {
    const auto* s = core::schema::script();
    if (!s) return;
    lily_row(1, world::p1_pawn(), *s);
    for (const auto& seat : party::seats()) lily_row(static_cast<std::uint16_t>(seat.slot), core::pawn(seat.slot), *s);
}

// adapter enemy (host half): spawn-point enemies keyed by the spawn point name
void write_enemies(HostCtx&) {
    const auto* s = core::schema::script();
    if (!s) return;
    resolve();
    if (g_resolved)
        for (const auto& e : registry::bucket(registry::Kind::Enemy)) {
            auto* enemy = e.handle.get();
            auto* sp = enemy ? g_spawn_point.object(enemy) : nullptr;
            if (!sp) continue; // dynamic enemies: section ed (adapter enemy_dyn)
            const auto spn = ue::utf8_name(sp);
            auto* row = g_frame->e.add(name(spn));
            if (!row) break;
            row->flags = enemy_flags(enemy);
            row->instance = static_cast<std::uint32_t>(ObjHandle::of(enemy).serial);
            if (auto at = core::location(enemy)) row->x = at->x, row->y = at->y, row->z = at->z;
            if (spn == g_trace_name && g_trace.size() < 240) g_trace.push_back(row->y); // Dev: raw host Y per frame
            if (auto* hp = s->zc_hp.object(enemy)) {
                const auto cur = s->hp_curr.read<std::int32_t>(hp);
                row->hp = static_cast<std::uint16_t>(std::max(0, cur));
                row->mhp = static_cast<std::uint16_t>(std::max(0, s->hp_max.read<std::int32_t>(hp)));
                if (cur <= 0) row->flags |= 4; // dead only when the last sample says so (unload ≠ death)
            }
            row->type = e.class_id;
            // bit3：敌人已拿到目标（原版 BattleColor 红光 / FindEnemy FX 的触发条件）
            if (row->flags & 8) ++g_engaged_rows;
            if (auto* loco = s->zc_locomotion.object(enemy)) {
                reflect::Frame fr(s->loco_facing_right);
                if (fr.invoke(loco)) row->facing = fr.ret_bool() ? 1 : -1;
            }
            presentation::capture(enemy, name, row->pose);
            visual_state::capture(enemy, row->visual);
        }
}

namespace {
void capture(std::int64_t now_us) {
    const auto t0 = clock::now_us();
    const auto* s = core::schema::script();
    g_frame->clear();
    g_frame->t_ms = static_cast<std::uint32_t>(now_us / 1000);
    g_frame->epoch = g_epoch;
    g_frame->map = name(ue::utf8(world::current_map_id()));
    if (!s) return;
    // adapters (lily / enemy / enemy_dyn / worldobj / story / pickup); per-map bookkeeping resets on a map change
    if (g_adapter_map != g_frame->map) {
        presentation::reset();
        visual_state::reset();
        for (auto* a : adapters()) a->reset();
        g_adapter_map = g_frame->map;
    }
    HostCtx ctx{*g_frame, now_us, [](std::string_view n) { return name(n); }};
    for (auto* a : adapters()) a->write_host(ctx);
    h_capture.add(static_cast<double>(clock::now_us() - t0));
}

// Action channel: launches captured by the ability detour (game thread) are sent reliably before the
// snapshot of the same frame; names go out as NameDef first (same reliable stream → ordered).
// P6: every replicated actor kind is an owner (spawn-point enemy, dynamic enemy, lily, spirit); events go to
// the peers whose seat stands in the owner's map and never back to the seat that produced them (its own
// lily / spirits run locally on that client).
struct Ref {
    proto::RefKind kind{};
    std::uint16_t id = 0;
    int slot = 0;
    std::uint8_t seat = 0; // Companion ownership survives row retirement for deferred FX/ability stops.
};

std::vector<std::pair<ue::UObject*, std::uint16_t>> g_lily_pawns; // refreshed once per flush

void refresh_lily_pawns() {
    g_lily_pawns.clear();
    if (auto* p1 = world::p1_pawn()) g_lily_pawns.emplace_back(p1, 1);
    for (const auto& seat : party::seats())
        if (auto* p = core::pawn(seat.slot)) g_lily_pawns.emplace_back(p, static_cast<std::uint16_t>(seat.slot));
}

std::optional<Ref> ref_of(ue::UObject* owner) {
    if (!ue::alive(owner)) return std::nullopt;
    Ref r;
    if (auto at = core::location(owner)) r.slot = std::max(0, maps::slot_of_y(at->y));
    for (const auto& [p, seat] : g_lily_pawns)
        if (p == owner) return r.kind = proto::RefKind::Lily, r.id = seat, r;
    const auto h = ObjHandle::of(owner);
    // SourceSpawnPoint is a ZenithCharacter property: other owners must not be read with it (access violation
    // 0x40000000d when a lily's spirit launched an ability)
    static ue::UClass* zc = reflect::find_class(L"/Script/Zenith.ZenithCharacter");
    if (!zc || !owner->GetClassPrivate()->IsChildOf(zc) || !g_resolved) return std::nullopt;
    if (auto* sp = g_spawn_point.object(owner)) return r.kind = proto::RefKind::Enemy, r.id = name(ue::utf8_name(sp)), r;
    if (const auto d = entities::dyn_id_of(h)) return r.kind = proto::RefKind::Dyn, r.id = d, r;
    return std::nullopt;
}

// FX / 音效的 owner 可能不是直接可复制的 Actor（伴随精灵、技能、从属对象）：沿 Owner 链找到第一个能映射
// 成 Ref 的角色。伴随精灵的自身发光（PS_Spirit）就是这种情况 —— 不这样做它们会整批 noOwner 丢掉。
std::optional<Ref> ref_of_owner(ue::UObject* o) {
    if (auto r = ref_of(o)) return r;
    static ue::UClass* actor = reflect::find_class(L"/Script/Engine.Actor");
    static reflect::FuncRef get_owner;
    if (actor && !get_owner.ok()) get_owner = reflect::Resolver("Actor", actor).func(L"GetOwner", 0);
    for (int i = 0; o && get_owner.ok() && i < 4; ++i) {
        reflect::Frame f(get_owner);
        if (!f.invoke(o)) break;
        auto* up = f.ret<ue::UObject*>();
        if (!up) break;
        if (auto r = ref_of(up)) return r;
        o = up;
    }
    return std::nullopt;
}

bool wants(const Peer& p, const Ref& r) {
    if (r.kind == proto::RefKind::Lily || r.kind == proto::RefKind::Spirit || r.kind == proto::RefKind::Companion) return false;
    if (!p.slot || std::max(0, maps::slot_of_seat(p.slot)) != r.slot) return false;
    if (r.kind == proto::RefKind::Lily && r.id == p.slot) return false;
    if (r.kind == proto::RefKind::Spirit && (r.id >> 8) == p.slot) return false;
    if (r.kind == proto::RefKind::Companion && r.seat == p.slot) return false;
    return true;
}

std::uint32_t ref_key(const Ref& r) { return static_cast<std::uint32_t>(r.kind) << 16 | r.id; }

std::uint64_t g_acts_sent = 0, g_acts_dropped = 0, g_fx_sent = 0, g_fx_stops_sent = 0, g_snd_sent = 0, g_ab_stops_sent = 0, g_ev_dropped = 0, g_ev_noref = 0;

template <class Msg>
void send_to(const Ref& r, const Msg& m, std::uint64_t& counter) {
    const auto n = proto::encode(m, g_buf);
    if (!n) return;
    for (auto& [id, peer] : g_peers)
        if (wants(peer, r)) send_rel(id, n), ++counter;
}

struct Pending {
    enum class Kind { Launch, Stop, ChildStop } kind = Kind::Launch;
    std::uint64_t order = 0, activation = 0, root = 0;
    ObjHandle owner, spawned;
    ability::Launch l;
    proto::AbilityPath path;
    std::string child_class;
    bool supported = true;
    std::uint8_t reason = 0;
};
std::vector<Pending> g_pending;
std::unordered_map<std::uint32_t, std::uint32_t> g_act_seq; // ref key → last act seq
struct AbilityDelivery {
    std::uint32_t peer = 0;
    std::uint16_t epoch = 0, map = 0;
};
struct AbilityLive {
    ObjHandle h;
    Ref r;
    std::uint32_t seq = 0;
    std::vector<AbilityDelivery> delivered;
    bool root_stop_sent = false;
};
// A family ledger, not an Actor-lifetime binding: FFA children can outlive or survive pool reuse of
// their root. Each child keeps this immutable root token until its own native activation ends.
std::unordered_map<std::uint64_t, AbilityLive> g_ab_live;
constexpr std::size_t kAbilityPending = 512, kAbilityLive = 1024;
std::string g_lifecycle_fault;
std::uint64_t g_ab_reused = 0, g_ab_expired = 0, g_ab_unmatched_stops = 0;
std::uint64_t g_ab_child_stops = 0, g_ab_child_unmatched = 0, g_ab_families_released = 0;

void lifecycle_fault(const char* reason) {
    if (g_lifecycle_fault.empty()) g_lifecycle_fault = reason;
}

void on_launch(const ability::Launch& l) {
    if (g_peers.empty()) return;
    if (!l.activation || g_pending.size() >= kAbilityPending) {
        ++g_acts_dropped;
        return lifecycle_fault("ability activation capture unavailable or overloaded");
    }
    Pending p;
    p.order = p.activation = l.activation; // assigned at native Launch entry, NOT Description return
    p.owner = ObjHandle::of(l.owner);
    p.spawned = ObjHandle::of(l.spawned);
    p.l = l;
    g_pending.push_back(std::move(p));
}

bool ability_delivery_current(const AbilityDelivery& delivery) {
    const auto it = g_peers.find(delivery.peer);
    return it != g_peers.end() && it->second.slot && it->second.epoch == delivery.epoch &&
           it->second.map_sent == delivery.map && it->second.map_ready == delivery.map;
}

void send_ability_stop(AbilityLive& live, std::uint8_t reason) {
    const auto n = proto::encode(proto::ActStop{static_cast<std::uint8_t>(live.r.kind), live.r.id, live.seq, reason}, g_buf);
    if (!n) return lifecycle_fault("ability stop encoding failed");
    // Map/session reset already stopped the old family. Never send its stop into the next map.
    for (const auto& delivery : live.delivered)
        if (ability_delivery_current(delivery)) send_rel(delivery.peer, n), ++g_ab_stops_sent;
    live.root_stop_sent = true;
}

void send_child_stop(const AbilityLive& live, const Pending& p) {
    if (!p.supported) return lifecycle_fault("ability child stop has no proven native Entries path");
    proto::ChildStop m;
    m.kind = static_cast<std::uint8_t>(live.r.kind);
    m.net_id = live.r.id;
    m.seq = live.seq;
    m.ability = p.child_class.empty() ? 0 : name(p.child_class);
    m.path = p.path;
    m.reason = p.reason;
    const auto n = proto::encode(m, g_buf);
    if (!n) return lifecycle_fault("ability child stop identity/encoding failed");
    for (const auto& delivery : live.delivered)
        if (ability_delivery_current(delivery)) send_rel(delivery.peer, n), ++g_ab_child_stops;
}

void send_acts() {
    // Invalidate deliveries permanently when their map/session leaves. Merely testing at send time
    // would revive an old binding if the same peer later returned to the same map name.
    for (auto it = g_ab_live.begin(); it != g_ab_live.end();) {
        std::erase_if(it->second.delivered, [](const AbilityDelivery& d) { return !ability_delivery_current(d); });
        if (!it->second.delivered.empty()) { ++it; continue; }
        ++g_ab_families_released;
        it = g_ab_live.erase(it);
    }
    auto pending = std::move(g_pending);
    g_pending.clear();
    std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.order < b.order; });
    for (auto& p : pending) {
        if (p.kind == Pending::Kind::ChildStop) {
            const auto it = g_ab_live.find(p.root);
            if (it == g_ab_live.end()) { ++g_ab_child_unmatched; continue; } // root had no recipients
            send_child_stop(it->second, p);
            if (!g_lifecycle_fault.empty()) return;
            continue;
        }
        if (p.kind == Pending::Kind::Stop) {
            auto it = g_ab_live.find(p.activation);
            if (it == g_ab_live.end() || it->second.h != p.spawned) { ++g_ab_unmatched_stops; continue; }
            send_ability_stop(it->second, p.reason);
            if (!g_lifecycle_fault.empty()) return;
            continue;
        }
        const auto r = ref_of(p.owner.get());
        if (!r) continue;
        if (g_ab_live.contains(p.activation)) continue;
        // Only the old root Actor ends on pool reuse. Its FFA children still address the old family;
        // neither their ledger nor a late old Stop may become the new root's wire identity.
        for (auto& [token, live] : g_ab_live) {
            if (live.h != p.spawned || live.root_stop_sent) continue;
            send_ability_stop(live, 2); // EAbilityStopReason::CANCEL
            if (!g_lifecycle_fault.empty()) return;
            ++g_ab_reused;
        }
        if (g_ab_live.size() >= kAbilityLive) return lifecycle_fault("ability activation table overloaded");
        auto act = p.l.act;
        act.kind = static_cast<std::uint8_t>(r->kind);
        act.net_id = r->id;
        act.ability = name(p.l.cls);
        act.custom_spawn = p.l.custom_spawn == "None" ? 0 : name(p.l.custom_spawn);
        act.custom_attach = p.l.custom_attach == "None" ? 0 : name(p.l.custom_attach);
        const auto event_map = name(r->slot == 0 ? ue::utf8(world::current_map_id()) : maps::map_id(r->slot));
        auto& seq = g_act_seq[ref_key(*r)];
        if (seq == ~std::uint32_t{0}) return lifecycle_fault("ability wire sequence exhausted");
        act.seq = ++seq;
        if (!act.ability || !event_map) return lifecycle_fault("ability launch asset/map identity unavailable");
        const auto n = proto::encode(act, g_buf);
        if (!n) { ++g_acts_dropped; continue; }
        AbilityLive live{p.spawned, *r, act.seq, {}};
        for (auto& [id, peer] : g_peers)
            if (wants(peer, *r) && peer.map_sent == event_map && peer.map_ready == event_map) {
                send_rel(id, n);
                live.delivered.push_back({id, peer.epoch, event_map});
                ++g_acts_sent;
            }
        if (!live.delivered.empty()) g_ab_live.emplace(p.activation, std::move(live));
    }
    for (auto it = g_ab_live.begin(); it != g_ab_live.end();) {
        auto& live = it->second;
        auto* root = live.h.get();
        const bool same_root = root && ability::activation(root) == it->first;
        const bool family_active = ability::has_active_family(it->first);
        if (!live.root_stop_sent && (!same_root || !family_active)) {
            send_ability_stop(live, 3); // END_PLAY only addresses the old root, never its children/new use
            if (!g_lifecycle_fault.empty()) return;
            ++g_ab_expired;
        }
        if (family_active) { ++it; continue; }
        ++g_ab_families_released;
        it = g_ab_live.erase(it);
    }
}

// The local predicted Lily has no remote presentation Ref on its own Client. Host-accepted reactions
// therefore route directly to the network seat, never through wants() (which excludes that seat).
struct ReactionPending {
    std::uint32_t peer = 0;
    ObjHandle owner;
    reaction::Capture capture;
};
constexpr std::size_t kReactionPending = 128;
std::vector<ReactionPending> g_reactions;
std::uint32_t g_reaction_seq = 0; // process lifetime; do not reuse a sequence after role stop
std::uint64_t g_reaction_order = 0; // last flushed native entry ordinal, shared across reaction kinds
std::uint64_t g_reaction_sent = 0, g_reaction_stale = 0, g_reaction_failed = 0;

std::uint16_t reaction_seat(ue::UObject* owner) {
    if (!ue::alive(owner)) return 0;
    if (owner == world::p1_pawn()) return 1;
    for (const auto& seat : party::seats())
        if (seat.slot > 1 && seat.slot <= 8 && core::pawn(seat.slot) == owner) return static_cast<std::uint16_t>(seat.slot);
    return 0;
}
bool wants_reaction(ue::UObject* owner) { return !g_peers.empty() && reaction_seat(owner) != 0; }

void on_reaction(const reaction::Capture& capture) {
    const auto seat = reaction_seat(capture.owner);
    const auto at = core::location(capture.owner);
    if (!seat || !at) return void(++g_reaction_stale);
    if (!capture.entry_order || g_reactions.size() >= kReactionPending) return lifecycle_fault("reaction capture order unavailable or overloaded");
    ReactionPending p;
    p.owner = ObjHandle::of(capture.owner); p.capture = capture; p.capture.owner = nullptr;
    auto& event = p.capture.event;
    const int slot = maps::slot_of_y(at->y);
    event.seat = static_cast<std::uint8_t>(seat);
    event.map = name(slot == 0 ? ue::utf8(world::current_map_id()) : maps::map_id(slot));
    if (event.kind == 0) event.position[1] -= maps::offset(slot);
    g_reactions.push_back(std::move(p));
}
void send_reactions() {
    auto pending = std::move(g_reactions); g_reactions.clear();
    std::sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) { return a.capture.entry_order < b.capture.entry_order; });
    for (auto& p : pending) {
        if (p.capture.entry_order <= g_reaction_order) return lifecycle_fault("reaction native entry order repeated or reversed");
        g_reaction_order = p.capture.entry_order;
        auto& event = p.capture.event;
        if (!p.owner.valid() || reaction_seat(p.owner.get()) != event.seat) { ++g_reaction_stale; continue; }
        if (g_reaction_seq == ~std::uint32_t{0} || p.capture.states.size() != event.state_count) return lifecycle_fault("reaction sequence/state capture invalid");
        event.seq = ++g_reaction_seq;
        event.curve = event.kind == 0 && !p.capture.curve.empty() ? name(p.capture.curve) : 0;
        for (std::size_t i = 0; i < event.state_count; ++i) event.states[i] = name(p.capture.states[i]);
        for (auto& [id, peer] : g_peers) {
            if (!peer.slot || peer.map_sent != event.map || peer.map_ready != event.map) continue;
            event.epoch = peer.epoch;
            const auto n = proto::encode(event, g_buf);
            if (!n) return lifecycle_fault("reaction asset identity or encoding failed");
            send_rel(id, n); ++g_reaction_sent;
        }
    }
}

// One activation of one host PSC owns one wire ID. Pending delivery, owner-resolution retry,
// late-join state and stop all refer to this record; pooled PSC reuse creates a new activation ID.
// Invalidators: native DeactivateSystem, inactive/destroyed component, owner/asset destruction,
// one-shot owner-resolution timeout, peer/map/ref departure, and session stop. Persistent records stay
// bounded by kFxCapacity while their weak owner/asset/PSC remain live. Audited in describe().
struct FxDelivery {
    std::uint32_t peer = 0;
    std::uint16_t map = 0, net_id = 0;
    proto::RefKind kind = proto::RefKind::Enemy;
    std::int64_t next_baseline_us = 0;
};
struct FxLive {
    ObjHandle owner, ps, psc;
    proto::Fx msg;
    std::string custom_bone;
    std::string attach_component, attach_socket;
    std::vector<FxDelivery> delivered;
    std::uint16_t waiting = 0;
    bool stopped = false, first_flush = true, ever_delivered = false;
};
constexpr std::size_t kFxCapacity = 1024;
constexpr std::int64_t kFxBaselineUs = 1'000'000;
std::unordered_map<std::uint32_t, FxLive> g_fx_live; // wire ID -> activation; stopped entries remain until their stops are flushed
std::unordered_map<ObjHandle, std::uint32_t> g_fx_by_psc; // weak component -> current activation only
std::uint32_t g_fx_seq = 0; // not reset on role stop; IDs are never reused by this process
std::uint64_t g_fx_canceled = 0, g_fx_retired = 0, g_fx_initial = 0, g_fx_retry_ready = 0;
std::uint64_t g_fx_baselines = 0, g_fx_bindings_closed = 0, g_fx_companion_owned = 0, g_fx_native_owned = 0;
struct SndEv {
    ObjHandle owner, event;
    proto::Snd msg;
    int slot = -1;
    std::string map;
};
std::vector<SndEv> g_snd_pending;
std::uint64_t g_snd_map_retired = 0;
std::uint64_t g_feedback_fx_sent = 0, g_feedback_sound_sent = 0, g_feedback_own_sent = 0;

bool wants_feedback(const Peer& peer, const Ref& r, bool feedback) {
    return wants(peer, r) || (feedback && r.kind == proto::RefKind::Lily && peer.slot &&
                             std::max(0, maps::slot_of_seat(peer.slot)) == r.slot);
}

void on_fx(const fx::Spawn& s) {
    if (entities::companion_owns_fx(s.owner, s.psc)) return void(++g_fx_companion_owned);
    if (!(s.msg.flags & 32) && fx::native_ability_owned(s.psc)) return void(++g_fx_native_owned); // Host-only effects have a different writer
    if (g_peers.empty() && !(s.msg.flags & 4)) return;
    if (!ue::alive(s.owner) || !ue::alive(s.ps) || !fx::active(s.psc)) return void(++g_ev_dropped);
    const auto psc = ObjHandle::of(s.psc);
    if (auto old = g_fx_by_psc.find(psc); old != g_fx_by_psc.end()) {
        if (auto it = g_fx_live.find(old->second); it != g_fx_live.end()) it->second.stopped = true;
        g_fx_by_psc.erase(old);
    }
    // Never evict an already-delivered record to admit a spawn: its Stop still needs to be sent.
    if (g_fx_live.size() >= kFxCapacity || g_fx_seq == ~std::uint32_t{0}) {
        ++g_ev_dropped;
        if (s.msg.flags & 32) lifecycle_fault("authoritative feedback FX capacity exhausted");
        return;
    }
    FxLive record;
    record.owner = ObjHandle::of(s.owner);
    record.ps = ObjHandle::of(s.ps);
    record.psc = psc;
    record.msg = s.msg;
    record.msg.t_ms = static_cast<std::uint32_t>(clock::now_us()/1000);
    record.msg.causal_root = causality::current_root();
    if(record.msg.flags&32)causality::captured(record.msg.causal_root,record.msg.t_ms,false);
    record.msg.seq = ++g_fx_seq;
    record.custom_bone = s.custom_bone;
    record.attach_component = s.attach_component;
    record.attach_socket = s.attach_socket;
    g_fx_by_psc[psc] = record.msg.seq;
    g_fx_live.emplace(record.msg.seq, std::move(record));
}
void on_deactivate(ue::UObject* psc) {
    const auto h = ObjHandle::of(psc);
    auto current = g_fx_by_psc.find(h);
    if (current == g_fx_by_psc.end()) return;
    if (auto it = g_fx_live.find(current->second); it != g_fx_live.end()) it->second.stopped = true;
    g_fx_by_psc.erase(current);
}
void on_sound(const fx::Sound& s) {
    if (g_peers.empty()) return;
    if (g_snd_pending.size() >= 128) {
        ++g_ev_dropped;
        if (s.flags & 1) lifecycle_fault("authoritative feedback sound queue exhausted");
        return;
    }
    proto::Snd msg;
    msg.t_ms = static_cast<std::uint32_t>(clock::now_us()/1000);
    msg.causal_root = causality::current_root();
    if(s.flags&1)causality::captured(msg.causal_root,msg.t_ms,true);
    msg.is2d = static_cast<std::uint8_t>(s.is2d);
    msg.flags = s.flags;
    msg.xf = s.xf;
    const auto at = core::location(s.owner);
    const int slot = at ? maps::slot_of_y(at->y) : -1;
    const auto map = slot == 0 ? ue::utf8(world::current_map_id()) : maps::map_id(slot);
    if (slot < 0 || map.empty()) {
        ++g_ev_dropped;
        if (s.flags & 1) lifecycle_fault("authoritative feedback sound capture map unavailable");
        return;
    }
    // Capture map identity before maps::tick can move this Actor. Queue no transport/name-table
    // work from the native hook; map departure retires the old event instead of rebasing its point.
    if ((msg.flags & 2) && !msg.is2d) msg.xf[5] -= maps::offset(slot);
    g_snd_pending.push_back({ObjHandle::of(s.owner), ObjHandle::of(s.event), msg, slot, map});
}
void on_ability_stop(ue::UObject* object, std::uint8_t reason) {
    if (g_peers.empty()) return;
    ability::Identity identity;
    if (!ability::identity(object, identity)) return; // snapshot BEFORE native Stop / any nested reuse
    const auto order = ability::next_capture_order();
    if (!order || g_pending.size() >= kAbilityPending) {
        ++g_ev_dropped;
        return lifecycle_fault("ability stop capture overloaded");
    }
    Pending p;
    p.kind = identity.token == identity.root ? Pending::Kind::Stop : Pending::Kind::ChildStop;
    p.order = order;
    p.activation = identity.token;
    p.root = identity.root;
    p.spawned = ObjHandle::of(object);
    p.path = identity.path;
    p.child_class = std::move(identity.cls);
    p.supported = identity.supported;
    p.reason = reason;
    g_pending.push_back(std::move(p));
}

void send_fx_stop(std::uint32_t peer, std::uint32_t seq) {
    if (auto it = g_peers.find(peer); it != g_peers.end() && it->second.slot) {
        if (const auto n = proto::encode(proto::FxStop{seq,static_cast<std::uint32_t>(clock::now_us()/1000)}, g_buf)) send_rel(peer, n), ++g_fx_stops_sent;
    }
}

void flush_fx(std::int64_t now_us) {
    for (auto it = g_fx_live.begin(); it != g_fx_live.end();) {
        const auto seq = it->first;
        auto& e = it->second;
        auto* owner = e.owner.get();
        auto* ps = e.ps.get();
        const bool feedback = (e.msg.flags & 32) != 0;
        if (!owner || !ps || !fx::active(e.psc.get())) e.stopped = true;
        // SpawnEmitterAttached returns before BP assigns its FX field. Check again after that assignment
        // so the exact companion baseline PSC is never also kept in the generic event stream.
        if (!e.stopped && entities::companion_owns_fx(owner, e.psc.get())) e.stopped = true, ++g_fx_companion_owned;
        if (!e.stopped && !feedback && fx::native_ability_owned(e.psc.get())) e.stopped = true, ++g_fx_native_owned;
        const bool persistent = (e.msg.flags & 4) != 0;
        const auto r = !e.stopped ? ref_of_owner(owner) : std::nullopt;
        if (!e.stopped && !r) {
            if (!e.waiting) ++g_ev_noref;
            if (e.waiting <= 120) ++e.waiting;
            for (const auto& delivery : e.delivered) send_fx_stop(delivery.peer, seq), ++g_fx_bindings_closed;
            e.delivered.clear();
            if (e.waiting > 120 && feedback) return lifecycle_fault("authoritative feedback FX owner unavailable");
            if (!persistent && e.waiting > 120) e.stopped = true, ++g_ev_dropped;
        }
        if (e.stopped) {
            for (const auto& delivery : e.delivered) send_fx_stop(delivery.peer, seq);
            if (!e.ever_delivered) ++g_fx_canceled;
            ++g_fx_retired;
            if (auto at = g_fx_by_psc.find(e.psc); at != g_fx_by_psc.end() && at->second == seq) g_fx_by_psc.erase(at);
            it = g_fx_live.erase(it);
            continue;
        }
        if (!r) { ++it; continue; }
        if (e.waiting) ++g_fx_retry_ready, e.waiting = 0;
        const auto map = r->slot == 0 ? g_frame->map : name(maps::map_id(r->slot));
        // A live PSC can outlast its presented owner identity. Stop the old binding before reusing the
        // same activation ID with a new Ref/map; a missing Ref above likewise clears every delivery.
        std::erase_if(e.delivered, [&](const FxDelivery& d) {
            auto peer = g_peers.find(d.peer);
            if (peer == g_peers.end()) return true;
            if (d.map == map && d.kind == r->kind && d.net_id == r->id && peer->second.map_sent == map &&
                peer->second.map_ready == map && wants_feedback(peer->second, *r, feedback)) return false;
            send_fx_stop(d.peer, seq);
            ++g_fx_bindings_closed;
            return true;
        });
        if (e.first_flush || persistent) {
            auto m = e.msg;
            m.kind = static_cast<std::uint8_t>(r->kind);
            m.net_id = r->id;
            bool named = false;
            for (auto& [id, peer] : g_peers) {
                if (!wants_feedback(peer, *r, feedback) || peer.map_sent != map || peer.map_ready != map) continue;
                FxDelivery* delivery = nullptr;
                for (auto& d : e.delivered) if (d.peer == id) { delivery = &d; break; }
                if (delivery && (!persistent || now_us < delivery->next_baseline_us)) continue;
                if (!named) {
                    m.ps = name(fx::path_of(ps));
                    m.custom_bone = e.custom_bone.empty() || e.custom_bone == "None" ? 0 : name(e.custom_bone);
                    m.attach_component = e.attach_component.empty() ? 0 : name(e.attach_component);
                    m.attach_socket = e.attach_socket.empty() || e.attach_socket == "None" ? 0 : name(e.attach_socket);
                    if ((!e.attach_component.empty() && !m.attach_component) || (!e.attach_socket.empty() && e.attach_socket != "None" && !m.attach_socket) ||
                        (!e.custom_bone.empty() && e.custom_bone != "None" && !m.custom_bone) || !m.ps) {
                        if (feedback) return lifecycle_fault("authoritative feedback FX asset identity unavailable");
                        break;
                    }
                    named = true;
                }
                if (!e.first_flush) m.flags |= 16; // late join / recovery / periodic baseline always reuse seq
                auto delivered=m;
                if(delivered.causal_root&&causality::source_seat(delivered.causal_root)==peer.slot)delivered.causal_root=0;
                if(delivered.causal_root&&!peer.native_roots.contains(delivered.causal_root))continue;
                if (const auto n = proto::encode(delivered, g_buf)) {
                    send_rel(id, n);
                    if (delivery) delivery->next_baseline_us = now_us + kFxBaselineUs, ++g_fx_baselines;
                    else {
                        e.delivered.push_back({id, map, r->id, r->kind, now_us + kFxBaselineUs});
                        if (!e.first_flush) ++g_fx_initial;
                    }
                    e.ever_delivered = true;
                    ++g_fx_sent;
                    if (feedback) {
                        ++g_feedback_fx_sent;
                        if (r->kind == proto::RefKind::Lily && r->id == peer.slot) ++g_feedback_own_sent;
                    }
                } else if (feedback) return lifecycle_fault("authoritative feedback FX encoding failed");
            }
            e.first_flush = false;
        }
        ++it;
    }
}

void send_events(std::int64_t now_us) {
    flush_fx(now_us);
    for (auto& e : g_snd_pending) {
        auto* owner = e.owner.get();
        auto* ev = e.event.get();
        // maps::tick runs before this flush and may unload the captured source's map/Actor.
        // Retire that lifecycle first; an expired owner is not an unsupported live feedback source.
        if (!owner || !ev || maps::map_id(e.slot) != e.map) { ++g_snd_map_retired; continue; }
        const auto at = core::location(owner);
        if (at && maps::slot_of_y(at->y) != e.slot) { ++g_snd_map_retired; continue; }
        const auto r = ref_of_owner(owner);
        if (!r) {
            ++g_ev_noref;
            if (e.msg.flags & 1) return lifecycle_fault("authoritative feedback sound owner unavailable");
            continue;
        }
        auto msg = e.msg;
        const auto current_map = r->slot == 0 ? ue::utf8(world::current_map_id()) : maps::map_id(r->slot);
        if (r->slot != e.slot || current_map != e.map) { ++g_snd_map_retired; continue; }
        msg.kind = static_cast<std::uint8_t>(r->kind);
        msg.net_id = r->id;
        msg.event = name(fx::path_of(ev));
        const auto map = name(e.map);
        const auto n = proto::encode(msg, g_buf);
        if (!n || !map) {
            ++g_ev_dropped;
            if (msg.flags & 1) return lifecycle_fault("authoritative feedback sound encoding/map failed");
            continue;
        }
        for (auto& [id, peer] : g_peers)
            if (wants_feedback(peer, *r, msg.flags & 1) && peer.map_sent == map && peer.map_ready == map) {
                auto delivered=msg;
                if(delivered.causal_root&&causality::source_seat(delivered.causal_root)==peer.slot)delivered.causal_root=0;
                if(delivered.causal_root&&!peer.native_roots.contains(delivered.causal_root))continue;
                const auto count=proto::encode(delivered,g_buf);if(!count)return lifecycle_fault("causal sound encoding failed");
                send_rel(id, count), ++g_snd_sent;
                if (msg.flags & 1) {
                    ++g_feedback_sound_sent;
                    if (r->kind == proto::RefKind::Lily && r->id == peer.slot) ++g_feedback_own_sent;
                }
            }
    }
    g_snd_pending.clear();
}

// Per-peer view (P3, coop/maps): a peer receives the rows of the map its seat stands in, in that map's local
// coordinates (background slot k is captured at Y +300000·k). World rows are name keyed and kept whole.
template <class Sec>
void band(const Sec& in, Sec& out, int slot, float off) {
    out.clear();
    for (const auto& r : in.view())
        if (maps::slot_of_y(r.y) == slot)
            if (auto* o = out.add(r.key)) *o = r, o->y -= off;
}

std::unique_ptr<snap::Frame> g_view = std::make_unique<snap::Frame>();

void send_snapshots() {
    for (auto& [id, p] : g_peers) {
        if (!p.slot) continue;
        const int slot = std::max(0, maps::slot_of_seat(p.slot));
        const float off = maps::offset(slot);
        auto& v = *g_view;
        v.seq = ++p.seq;
        v.t_ms = g_frame->t_ms;
        v.epoch = g_frame->epoch;
        v.me = static_cast<std::uint8_t>(p.slot);
        v.map = slot == 0 ? g_frame->map : name(maps::map_id(slot));
        band(g_frame->l, v.l, slot, off);
        band(g_frame->e, v.e, slot, off);
        band(g_frame->ed, v.ed, slot, off);
        band(g_frame->pk, v.pk, slot, off);
        v.w = g_frame->w;
        const snap::Frame* base = nullptr;
        if (p.acked) {
            const auto& b = (*p.ring)[p.acked % kRing];
            if (b.seq == p.acked) base = &b;
        }
        const auto n = snap::encode(v, base, base ? p.acked : 0, g_buf);
        (*p.ring)[p.seq % kRing] = v;
        if (n) {
            coop::send_unreliable(id, proto::kSnapStream, std::span(g_buf.data(), n));
            ++c_snaps;
        } else ++c_snap_fail;
        if (p.map_sent != v.map) { // follow the seat's map (client replies MapReady)
            p.map_sent = v.map;
            p.map_ready = 0;
            ++p.native_view;
            p.native_frames.clear(); p.native_roots.clear(); p.native_retired.clear(); p.native_spirit_commands.clear(); p.native_map = 0; p.native_builds.fill({});
            std::string tag = slot == 0 ? std::string{} : maps::tag_of_seat(p.slot);
            if (slot == 0)
                if (auto* wl = world::world_loader())
                    if (auto* prop = reflect::find_property(static_cast<ue::UStruct*>(wl->GetClassPrivate()), L"PlayerStartTag"))
                        tag = ue::utf8(*reinterpret_cast<const ue::FName*>(reinterpret_cast<const std::uint8_t*>(wl) + prop->GetOffset_Internal()));
            send_rel(id, proto::encode(proto::MapMsg{v.map, tag, p.native_view}, g_buf));
        }
    }
}
void on_connected(std::uint32_t peer) {
    auto& p = g_peers[peer];
    p.id = peer;
    for (std::size_t i = 0; i < g_names.all().size(); ++i) // late join: replay the name table
        send_rel(peer, proto::encode(proto::NameDef{static_cast<std::uint16_t>(i + 1), g_names.all()[i]}, g_buf));
}

std::string seat_for(Peer& p) {
    if (p.slot) return {};
    if (auto it = g_parked.find(p.guest_key); !p.guest_key.empty() && it != g_parked.end() && party::seat(it->second)) {
        p.slot = it->second;
        g_parked.erase(it);
        log::info("coop: guest {} reclaimed seat {}", p.guest_key, p.slot);
    } else {
        party::JoinOptions o;
        o.source = party::Source::Network;
        if (auto e = party::join(o, &p.slot); !e.empty()) return e;
        log::info("coop: peer {} seated at P{}", p.id, p.slot);
    }
    p.epoch = ++g_epoch;
    send_rel(p.id, proto::encode(proto::Welcome{static_cast<std::uint8_t>(p.slot), p.epoch, g_frame->map}, g_buf));
    // flush_fx supplies persistent activations after MapReady. It reuses each existing ID and
    // remembers delivery per peer, so joining while a spawn is pending cannot duplicate it.
    return {};
}

void on_closed(std::uint32_t peer, const std::string& why, net::Reject) {
    auto it = g_peers.find(peer);
    if (it == g_peers.end()) return;
    auto& p = it->second;
    if (p.slot) {
        input::pad::set(p.slot, 0, 0, 0);
        if (why.find("peer closed") != std::string::npos || p.guest_key.empty()) party::leave(p.slot); // explicit leave
        else g_parked[p.guest_key] = p.slot, log::info("coop: P{} parked for guest {} ({})", p.slot, p.guest_key, why);
    }
    g_peers.erase(it);
}

void on_reliable(std::uint32_t peer, std::span<const std::uint8_t> msg) {
    auto it = g_peers.find(peer);
    if (it == g_peers.end()) return;
    auto& p = it->second;
    switch (proto::type_of(msg)) {
    case proto::Msg::Build: {
        proto::Json j;
        if (!proto::decode(msg, j)) return;
        json::Value root;
        if (!json::parse(j.text, root) || !root.is_object()) return;
        if (const auto* k = root.get("guestKey"); k && k->is_string() && k->str.size() <= 64) p.guest_key = k->str;
        const bool fresh_seat = !p.slot && !g_parked.contains(p.guest_key);
        if (auto e = seat_for(p); !e.empty()) {
            coop::send_reliable(peer, std::span(g_buf.data(), proto::encode(proto::Control{1, "no seat: " + e}, g_buf)));
            coop::kick(peer, "no seat: " + e);
            break;
        }
        // FR-NET-2c: the guest's own equipment on a fresh seat (a reclaimed seat keeps what it had); progress
        // stays the host's shared progress. Items the host's progress does not own are reported, not granted.
        if (const auto* b = root.get("build"); b && fresh_seat) {
            core::profile::Profile prof;
            std::string err;
            if (core::profile::from_json(*b, prof, &err)) {
                const auto r = core::profile::apply(core::controller(p.slot), prof, false);
                p.build_report = r.text();
                log::info("coop: P{} guest build applied: {}", p.slot, p.build_report);
            } else {
                p.build_report = "bad build: " + err;
            }
        }
        break;
    }
    case proto::Msg::EquipRequest: { // F2.10: the guest's equipment change, applied to its seat on the host
        proto::Json j;
        json::Value v;
        core::profile::Profile prof;
        std::string err, result;
        if (!p.slot || !proto::decode(msg, j) || !json::parse(j.text, v) || !core::profile::from_json(v, prof, &err)) result = "refused: " + (err.empty() ? "bad request" : err);
        else result = core::profile::apply(core::controller(p.slot), prof, false).text();
        p.build_report = result;
        std::vector<std::uint8_t> big(proto::kMaxJson + 64);
        if (const auto n = proto::encode(proto::Msg::EquipResult, proto::Json{result.substr(0, 1000)}, big)) coop::send_reliable(peer, std::span(big.data(), n));
        log::info("coop: P{} equipment request: {}", p.slot, result);
        break;
    }
    case proto::Msg::RefreshView: {
        proto::RefreshView request;
        if (p.slot && proto::decode(msg, request) && request.map == p.map_sent && request.view == p.native_view) {
            // The normal map handshake retires old deliveries and issues a full baseline for this peer.
            log::info("coop: refreshing playback for P{} map={} view={}", p.slot, p.map_sent, p.native_view);
            p.map_sent = p.map_ready = 0;
            p.acked = 0;
        }
        break;
    }
    case proto::Msg::MapReady: {
        proto::MapReady m;
        if (proto::decode(msg, m) && m.map == p.map_sent && m.view == p.native_view) p.map_ready = m.map;
        break;
    }
    default: break;
    }
}

void on_unreliable(std::uint32_t peer, std::uint8_t stream, std::span<const std::uint8_t> msg) {
    auto it = g_peers.find(peer);
    if (stream != proto::kInputStream || it == g_peers.end() || !it->second.slot) return;
    auto& p = it->second;
    proto::Input in;
    if (!proto::decode(msg, in) || in.epoch != p.epoch) return;
    p.acked = std::max(p.acked, std::min(in.snap_ack, p.seq));
    input::pad::set(p.slot, in.x / 100.0, in.y / 100.0, in.raw);
    for (int c = 1; c < 10; ++c) {
        if (p.presses_known && in.presses[c] != p.presses[c]) input::pad::tap(p.slot, 1u << c);
        p.presses[c] = in.presses[c];
    }
    p.presses_known = true;
    p.last_input_us = clock::now_us();
    p.stalled = false;
}

void native_frame(ue::UObject* pawn, proto::LilyFrame& f) {
    if (g_peers.empty()) return;
    const int slot = maps::slot_of_y(f.position[1]);
    if (slot < 0) return;
    f.map = name(slot == 0 ? ue::utf8(world::current_map_id()) : maps::map_id(slot));
    f.position[1] -= maps::offset(slot);
    for (auto& edge : f.commands)
        if (edge.climb_target) (*edge.climb_target)[1] -= maps::offset(slot);
    for (auto& [id, peer] : g_peers)
        if (peer.slot && peer.slot != f.seat && !peer.native_builds[f.seat].empty() && peer.native_map == f.map && maps::slot_of_seat(peer.slot) == slot && peer.map_ready == f.map && peer.map_sent == f.map)
            if (peer.native_frames.size() < 128) { auto copy=f; copy.epoch=peer.epoch; copy.view=peer.native_view; peer.native_frames.push_back(copy); }
            else lifecycle_fault("native Lily source frame queue capacity");
}
bool causal_place(std::uint16_t seat, std::uint16_t& map, float& offset) {
    auto* pawn=seat==1?world::p1_pawn():core::pawn(seat);auto at=core::location(pawn);if(!at)return false;
    const int slot=maps::slot_of_y(at->y);if(slot<0)return false;offset=maps::offset(slot);map=name(slot?maps::map_id(slot):ue::utf8(world::current_map_id()));return map!=0;
}
bool causal_reference(ue::UObject* actor, proto::Cause& fact, std::uint16_t map) {
    const auto ref=ref_of(actor);if(!ref||(ref->kind!=proto::RefKind::Enemy&&ref->kind!=proto::RefKind::Dyn))return false;
    const auto actual=name(ref->slot?maps::map_id(ref->slot):ue::utf8(world::current_map_id()));if(actual!=map)return false;
    fact.ref_kind=static_cast<std::uint8_t>(ref->kind);fact.ref_id=ref->id;fact.ref_generation=static_cast<std::uint32_t>(ObjHandle::of(actor).serial);return true;
}
std::vector<proto::SpiritFrame> g_spirit_frames;
void spirit_frame(proto::SpiritFrame& f) {
    std::uint16_t map=0;float offset=0;if(!causal_place(f.context.seat,map,offset))return;f.context.map=map;f.context.position[1]-=offset;if(f.has_follow){f.follow_position[1]-=offset;for(auto i:{1u,7u,10u})f.follow_history[i]-=offset;}
    if(g_spirit_frames.size()>=2048)return lifecycle_fault("autonomous source frame capacity");g_spirit_frames.push_back(f);
}
void flush_native(std::int64_t now) {
    auto facts=causality::take();static std::vector<std::uint8_t> bytes(net::Link::kMaxMessage);
    // host_tick can create a terminal Spirit frame after the outer Tick's timestamp.
    // Seal after all capture, including that final frame.
    const auto sealed=static_cast<std::uint32_t>(clock::now_us()/1000);
    for(auto& [id,p]:g_peers){
        if(!p.slot||!p.native_map||p.map_ready!=p.native_map||p.map_sent!=p.native_map)continue;
        proto::LilyCauses batch;batch.epoch=p.epoch;batch.map=p.native_map;batch.view=p.native_view;batch.fence_ms=sealed;
        for(const auto& f:g_spirit_frames)if(f.context.map==p.native_map&&maps::slot_of_seat(f.context.seat)==maps::slot_of_seat(p.slot)){
            for(const auto& e:f.edges)if(e.kind==0)p.native_spirit_commands[e.sequence]=now;
            if(p.native_spirit_commands.contains(f.command_sequence))p.native_spirit_commands[f.command_sequence]=now;
        }
        std::erase_if(p.native_spirit_commands,[&](const auto& e){return now-e.second>10'000'000;});
        if(p.native_spirit_commands.size()>4096)return lifecycle_fault("peer autonomous command capacity");
        for(const auto& f:facts){if(f.map!=p.native_map||(f.domain==0&&(f.seat==p.slot||p.native_builds[f.seat].empty())))continue;
            if(f.domain==1&&maps::slot_of_seat(f.seat)!=maps::slot_of_seat(p.slot))continue;
            if(f.domain==1&&f.kind==proto::CauseKind::Root&&!p.native_spirit_commands.contains(f.input))continue;
            if(f.kind==proto::CauseKind::Root)p.native_roots.insert(f.root);
            if(p.native_roots.contains(f.root))batch.facts.push_back(f);
        }
        // The last frame's playback interval can extend past its capture timestamp.
        // Keep sealing even when idle/paused: this grants no extra game delta or input.
        const auto n=proto::encode(batch,bytes);if(!n)return lifecycle_fault("causal batch encoding/capacity");coop::send_reliable(id,std::span(bytes.data(),n));
        for(const auto& f:p.native_frames){const auto n=proto::encode(f,bytes);if(!n)return lifecycle_fault("native Lily frame encoding");coop::send_reliable(id,std::span(bytes.data(),n));}p.native_frames.clear();
        for(auto f:g_spirit_frames)if(f.context.map==p.native_map&&maps::slot_of_seat(f.context.seat)==maps::slot_of_seat(p.slot)){f.context.epoch=p.epoch;f.context.view=p.native_view;const auto n=proto::encode(f,bytes);if(!n)return lifecycle_fault("autonomous frame encoding");coop::send_reliable(id,std::span(bytes.data(),n));}
        std::erase_if(p.native_roots,[&](auto root){if(causality::root_active(root)){p.native_retired.erase(root);return false;}const auto [it,fresh]=p.native_retired.try_emplace(root,now);if(now-it->second<2'000'000)return false;p.native_retired.erase(it);return true;});
    }
    g_spirit_frames.clear();
}

void send_native_builds(std::int64_t now_us) {
    static clock::Cadence cadence(500);
    if (!cadence.due(now_us)) return;
    for (std::uint16_t seat = 1; seat <= 8; ++seat) {
        auto* pawn = seat == 1 ? world::p1_pawn() : core::pawn(seat);
        auto* pc = seat == 1 ? world::p1_controller() : core::controller(seat);
        if (!pawn || !pc) continue;
        const auto at = core::location(pawn);
        if (!at) continue;
        const int slot = maps::slot_of_y(at->y);
        const auto map = name(slot == 0 ? ue::utf8(world::current_map_id()) : maps::map_id(slot));
        auto profile = core::profile::read(pc, nullptr);
        if (!profile) return lifecycle_fault("native Lily profile capture unavailable");
        const auto value = core::profile::to_json(*profile);
        const auto signature = json::dump(value, 0);
        for (auto& [id, peer] : g_peers) {
            if (!peer.slot || peer.slot == seat || peer.map_ready != map || peer.map_sent != map) continue;
            if (peer.native_map != map) { peer.native_builds.fill({}); peer.native_map = map; }
            if (peer.native_builds[seat] == signature) continue;
            json::Value body = json::Value::object();
            body.set("seat", static_cast<int>(seat)); body.set("map", static_cast<int>(map)); body.set("view", static_cast<std::int64_t>(peer.native_view)); body.set("profile", value);
            std::vector<std::uint8_t> bytes(proto::kMaxJson + 64);
            const auto n = proto::encode(proto::Msg::LilyBuild, proto::Json{json::dump(body, 0)}, bytes);
            if (!n) return lifecycle_fault("native Lily profile exceeds reliable packet limit");
            coop::send_reliable(id, std::span(bytes.data(), n));
            peer.native_builds[seat] = signature;
        }
    }
}

void tick(std::int64_t now_us) {
    causality::host_api({name,causal_place,causal_reference});
    if(const auto why=causality::fault();!why.empty())return coop::leave(why);
    if(!autonomous::host_tick(spirit_frame,name))return coop::leave(autonomous::fault());
    flush_native(now_us);
    if (const auto fault = fx::fault(); !fault.empty()) lifecycle_fault(fault.c_str());
    if (const auto fault = ability::fault(); !fault.empty()) lifecycle_fault(fault.c_str());
    if (const auto fault = reaction::host_fault(); !fault.empty()) lifecycle_fault(fault.c_str());
    if (!g_lifecycle_fault.empty()) return coop::leave(g_lifecycle_fault);
    if (lifecycle::state() != lifecycle::State::Settled) return;
    for (auto& [id, p] : g_peers)
        if (p.slot && !p.stalled && now_us - p.last_input_us > kStallUs) { // 500 ms without input: release keys
            input::pad::set(p.slot, 0, 0, 0);
            p.stalled = true;
        }
    capture(now_us);
    if (!native_lily::host_enable(native_frame, name)) return coop::leave("native Lily input observer unavailable");
    if (const auto why = native_lily::fault(); !why.empty()) return coop::leave(why);
    send_native_builds(now_us);
    if (!ability::host_enable(on_launch)) return coop::leave("host ability capture unavailable");
    if (!fx::host_enable({on_fx, on_deactivate, on_sound, on_ability_stop})) return coop::leave("host feedback capture unavailable");
    if (!reaction::host_enable(on_reaction, wants_reaction)) return coop::leave("host reaction capture unavailable");
    // 会话开始前（读图 / 敌人激活）就 spawn 的持续特效：补进在场集合，新对端 Welcome 时补发
    if (std::vector<fx::Spawn> early; fx::take_early(early) > 0)
        for (auto& s : early) on_fx(s);
    refresh_lily_pawns();
    party::transit::set_stay([](std::int64_t seat) { return maps::slot_of_seat(seat) > 0; });
    maps::tick(now_us);
    send_acts();
    send_reactions();
    if (!g_lifecycle_fault.empty()) return coop::leave(g_lifecycle_fault);
    send_events(now_us);
    send_snapshots();
}

void stop() {
    // Detach sinks before releasing seats: cleanup must not refill event queues for a dead session.
    ability::host_disable();
    fx::host_disable();
    reaction::host_disable();
    for (auto& [id, p] : g_peers)
        if (p.slot) party::leave(p.slot);
    for (auto& [k, slot] : g_parked) party::leave(slot);
    g_spirit_frames.clear();autonomous::reset();
    native_lily::host_disable();

    presentation::reset();
    visual_state::reset();
    g_snd_pending.clear(), g_fx_live.clear(), g_fx_by_psc.clear(), g_ab_live.clear();
    g_reactions.clear();
    g_lifecycle_fault.clear();
    party::transit::set_stay(nullptr);
    maps::reset();
    g_pending.clear();
    g_act_seq.clear();
    g_peers.clear();
    g_parked.clear();
    g_names.clear();
    fx::host_disable(); // discard any persistent FX produced by seat/map teardown after sinks were detached
}

} // namespace

void broadcast_banner(const std::string& text, std::int64_t only_seat) {
    for (auto& [id, p] : g_peers)
        if (p.slot && (only_seat == 0 || p.slot == only_seat)) send_rel(id, proto::encode(proto::Control{proto::kControlBanner, text}, g_buf));
}

std::string trace(const std::string& name) {
    std::string out;
    for (float y : g_trace) out += std::format("{:.1f} ", y);
    g_trace_name = name;
    g_trace.clear();
    return out.empty() ? "(tracing " + name + ")" : out;
}

std::string describe() {
    std::string out = std::format("host: peers={} parked={} names={} epoch={} actsSent={} actsDropped={}\n", g_peers.size(), g_parked.size(), g_names.size(), g_epoch,
                                  g_acts_sent, g_acts_dropped) +
                      std::format("events: fx={} fxStops={} sounds={} abilityStops={} dropped={} noOwner={} live fx={} abilities={} engaged={}\n", g_fx_sent, g_fx_stops_sent, g_snd_sent, g_ab_stops_sent, g_ev_dropped, g_ev_noref, g_fx_live.size(), g_ab_live.size(), g_engaged_rows) +
                      std::format("feedback delivery: fx={} sounds={} ownSeat={} soundMapRetired={}\n", g_feedback_fx_sent, g_feedback_sound_sent, g_feedback_own_sent, g_snd_map_retired) +
                      std::format("fx lifecycle: ids={} active={} canceledBeforeDelivery={} retired={} initialReplays={} retryReady={} baselines={} bindingsClosed={} companionOwned={} nativeAbilityOwned={}\n", g_fx_seq,
                                  g_fx_by_psc.size(), g_fx_canceled, g_fx_retired, g_fx_initial, g_fx_retry_ready, g_fx_baselines, g_fx_bindings_closed, g_fx_companion_owned, g_fx_native_owned) +
                      std::format("ability lifecycle: pending={} reused={} expired={} unmatchedStops={} childStops={} childUnmatched={} familiesReleased={} fault={}\n", g_pending.size(), g_ab_reused, g_ab_expired, g_ab_unmatched_stops, g_ab_child_stops, g_ab_child_unmatched, g_ab_families_released, g_lifecycle_fault) +
                      std::format("reactions: pending={} seq={} sent={} stale={} failed={}\n", g_reactions.size(), g_reaction_seq, g_reaction_sent, g_reaction_stale, g_reaction_failed) +
                      native_lily::describe() + ability::describe() + fx::describe() + reaction::describe();
    for (auto* a : adapters()) out += a->describe();
    out += maps::describe() + presentation::describe() + visual_state::describe();
    for (const auto& [id, p] : g_peers)
        out += std::format("  peer {} -> P{} key={} seq={} acked={} input={} mapReady={} build=[{}]\n", id, p.slot, p.guest_key, p.seq, p.acked, p.stalled ? "stalled" : "live", p.map_ready, p.build_report);
    return out;
}

void install() { coop::set_handlers(Role::Host, {on_connected, on_closed, on_reliable, on_unreliable, tick, stop}); }

} // namespace elmp::coop::host
