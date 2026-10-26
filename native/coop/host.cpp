// Coop host (§7.5 host/{capture,guest,events}): every connected peer gets a remote seat (party Source::Network)
// fed by its input stream; every frame the host captures one FrameSnapshot (lily rows: P1 + seats; enemy rows:
// registry Enemy bucket keyed by the SourceSpawnPoint name) and sends each peer a delta against the snapshot
// it acknowledged. Names travel once as NameDef (reliable). Disconnect without F_CLOSE keeps the seat parked
// under the guest key for reconnection (FR-NET-2c); an explicit leave removes it.

#include "coop/host.h"

#include "coop/ability.h"
#include "coop/session.h"

#include "party/seat.h"

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

#include <array>
#include <format>
#include <map>
#include <memory>
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
};

std::map<std::uint32_t, Peer> g_peers;
std::map<std::string, std::int64_t> g_parked; // guest key → seat kept after an unexpected disconnect
proto::NameTable g_names;
std::unique_ptr<snap::Frame> g_frame = std::make_unique<snap::Frame>();
std::array<std::uint8_t, 64 * 1024> g_buf;
std::uint16_t g_epoch = 1;
reflect::PropRef g_spawn_point;
bool g_resolved = false;
metrics::Counter& c_snaps = metrics::counter("coop.host.snapshots");
metrics::Histogram& h_capture = metrics::histogram("coop.host.capture_us");

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

// Spine track 0 (stage 1 of §4: GetCurrent + getAnimationName / GetTrackTime / GetLoop via ProcessEvent).
template <class Row>
void track0(ue::UObject* spine, Row& row, const core::schema::Script& s) {
    if (!spine) return;
    reflect::Frame cur(s.sp_get_current);
    cur.arg(0, 0);
    if (!cur.invoke(spine)) return;
    auto* entry = cur.ret<ue::UObject*>();
    if (!ue::alive(entry)) return;
    reflect::Frame nm(s.te_animation_name);
    if (nm.invoke(entry)) row.anim = name(log::utf8(nm.ret_string()));
    reflect::Frame t(s.te_track_time);
    if (t.invoke(entry)) row.t = t.ret<float>();
    reflect::Frame lp(s.te_loop);
    if (lp.invoke(entry)) row.loop = lp.ret_bool();
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
    if (const auto* bp = core::schema::blueprint()) track0(bp->lily_spine.object(pawn), *row, s);
}

// the enemy spine component is cached per enemy handle — invalidated when the handle dies
std::unordered_map<std::int32_t, std::pair<ObjHandle, ObjHandle>> g_spine; // enemy index → (enemy, spine comp)

void pose(ue::UObject* enemy, const ObjHandle& h, snap::EnemyRow& row, const core::schema::Script& s) {
    auto& c = g_spine[h.index];
    if (c.first != h) {
        static ObjHandle cls = ObjHandle::of(reflect::find_object(L"/Script/SpineExtensionPlugin.SpineSkeletonAnimationExComponent"));
        reflect::Frame f(s.actor_component_by_class);
        f.arg(0, cls.get());
        c = {h, f.invoke(enemy) ? ObjHandle::of(f.ret<ue::UObject*>()) : ObjHandle{}};
    }
    track0(c.second.get(), row, s);
}
// World objects (adapter worldobj, §7.5): interactables (doors / chests / switches / items) and breakables,
// keyed by actor name (level-placed → identical on both sides). Cleared = UClearableComponent::IsCleared,
// sampled at 2 Hz (ProcessEvent per object); destroyed objects of the current map stay as `gone` rows.
struct WorldObj {
    ObjHandle actor, clearable;
    std::uint8_t flags = 0;
};
std::unordered_map<std::string, WorldObj> g_world;
std::uint16_t g_world_map = 0;

void capture_world(std::int64_t now_us) {
    static clock::Cadence every(500);
    if (g_world_map != g_frame->map) g_world.clear(), g_world_map = g_frame->map, every.reset();
    if (every.due(now_us)) {
        static ObjHandle clear_cls = ObjHandle::of(reflect::find_object(L"/Script/Zenith.ClearableComponent"));
        static reflect::FuncRef is_cleared = reflect::Resolver("ClearableComponent", reflect::find_class(L"/Script/Zenith.ClearableComponent")).func(L"IsCleared", 0);
        const auto* s = core::schema::script();
        for (auto kind : {registry::Kind::Interactable, registry::Kind::Breakable})
            for (const auto& e : registry::bucket(kind)) {
                auto* a = e.handle.get();
                if (!a) continue;
                auto& w = g_world[ue::utf8_name(a)];
                if (w.actor != e.handle) {
                    w.actor = e.handle;
                    reflect::Frame f(s->actor_component_by_class);
                    f.arg(0, clear_cls.get());
                    w.clearable = f.invoke(a) ? ObjHandle::of(f.ret<ue::UObject*>()) : ObjHandle{};
                }
                w.flags &= ~2;
                if (auto* c = w.clearable.get(); c && is_cleared.ok()) {
                    reflect::Frame f(is_cleared);
                    if (f.invoke(c)) w.flags = f.ret_bool() ? (w.flags | 1) : (w.flags & ~1);
                }
                static reflect::PropRef breakable_hp =
                    reflect::Resolver("Breakable", reflect::find_class(L"/Script/Zenith.Breakable")).prop(L"HPComponent", reflect::Kind::Object);
                // ABreakable has its own HPComponent property (zc_hp is the ZenithCharacter offset — reading it on
                // a breakable crashed both processes: access violation 0x10000000b)
                if (kind == registry::Kind::Breakable && breakable_hp.ok())
                    if (auto* hp = breakable_hp.object(a); hp && s->hp_curr.read<std::int32_t>(hp) <= 0) w.flags |= 4; // broken (bit2; IsCleared alone does not mean broken)
            }
        for (auto& [n, w] : g_world)
            if (!w.actor.get()) w.flags |= 2; // destroyed on the host (breakable broken, item consumed)
    }
    for (const auto& [n, w] : g_world)
        if (w.flags)
            if (auto* row = g_frame->w.add(name(n))) row->flags = w.flags;
}

void capture(std::int64_t now_us) {
    const auto t0 = clock::now_us();
    const auto* s = core::schema::script();
    g_frame->clear();
    g_frame->t_ms = static_cast<std::uint32_t>(now_us / 1000);
    g_frame->epoch = g_epoch;
    g_frame->map = name(ue::utf8(world::current_map_id()));
    if (!s) return;
    lily_row(1, world::p1_pawn(), *s);
    for (const auto& seat : party::seats()) lily_row(static_cast<std::uint16_t>(seat.slot), core::pawn(seat.slot), *s);
    resolve();
    if (g_resolved)
        for (const auto& e : registry::bucket(registry::Kind::Enemy)) {
            auto* enemy = e.handle.get();
            auto* sp = enemy ? g_spawn_point.object(enemy) : nullptr;
            if (!sp) continue; // dynamic enemies: section ed (adapter enemy_dyn)
            auto* row = g_frame->e.add(name(ue::utf8_name(sp)));
            if (!row) break;
            if (auto at = core::location(enemy)) row->x = at->x, row->y = at->y, row->z = at->z;
            if (auto* hp = s->zc_hp.object(enemy)) {
                const auto cur = s->hp_curr.read<std::int32_t>(hp);
                row->hp = static_cast<std::uint16_t>(std::max(0, cur));
                row->mhp = static_cast<std::uint16_t>(std::max(0, s->hp_max.read<std::int32_t>(hp)));
                if (cur <= 0) row->flags |= 4; // dead only when the last sample says so (unload ≠ death)
            }
            row->type = e.class_id;
            pose(enemy, e.handle, *row, *s);
        }
    capture_world(now_us);
    h_capture.add(static_cast<double>(clock::now_us() - t0));
}

// Action channel: launches captured by the ability detour (game thread) are sent reliably before the
// snapshot of the same frame; names go out as NameDef first (same reliable stream → ordered).
struct Pending {
    ObjHandle owner;
    ability::Launch l;
};
std::vector<Pending> g_pending;
std::unordered_map<std::uint16_t, std::uint32_t> g_act_seq;
std::uint64_t g_acts_sent = 0, g_acts_dropped = 0;

void on_launch(const ability::Launch& l) {
    if (g_peers.empty()) return;
    if (g_pending.size() >= 64) return void(++g_acts_dropped);
    g_pending.push_back({ObjHandle::of(l.owner), l});
}

void send_acts() {
    for (auto& p : g_pending) {
        auto* owner = p.owner.get();
        // SourceSpawnPoint is a ZenithCharacter property: spirits / projectiles / other owners must not be
        // read with it (access violation 0x40000000d when a lily's spirit launched an ability)
        static ue::UClass* zc = reflect::find_class(L"/Script/Zenith.ZenithCharacter");
        if (!owner || !zc || !owner->GetClassPrivate()->IsChildOf(zc)) continue;
        auto* sp = g_resolved ? g_spawn_point.object(owner) : nullptr;
        if (!sp) continue; // only spawn-point enemies have a stable id (dynamic enemies: ed adapter)
        auto act = p.l.act;
        act.net_id = name(ue::utf8_name(sp));
        act.ability = name(p.l.cls);
        act.custom_spawn = p.l.custom_spawn == "None" ? 0 : name(p.l.custom_spawn);
        act.custom_attach = p.l.custom_attach == "None" ? 0 : name(p.l.custom_attach);
        act.seq = ++g_act_seq[act.net_id];
        if (!act.ability) continue;
        const auto n = proto::encode(act, g_buf);
        for (auto& [id, peer] : g_peers)
            if (peer.slot) send_rel(id, n), ++g_acts_sent;
    }
    g_pending.clear();
}

void send_snapshots() {
    for (auto& [id, p] : g_peers) {
        if (!p.slot) continue;
        g_frame->seq = ++p.seq;
        g_frame->me = static_cast<std::uint8_t>(p.slot);
        const snap::Frame* base = nullptr;
        if (p.acked) {
            const auto& b = (*p.ring)[p.acked % kRing];
            if (b.seq == p.acked) base = &b;
        }
        const auto n = snap::encode(*g_frame, base, base ? p.acked : 0, g_buf);
        (*p.ring)[p.seq % kRing] = *g_frame;
        if (n) {
            coop::send_unreliable(id, proto::kSnapStream, std::span(g_buf.data(), n));
            ++c_snaps;
        }
        if (p.map_sent != g_frame->map) { // follow the host map (client replies MapReady)
            p.map_sent = g_frame->map;
            std::string tag;
            if (auto* wl = world::world_loader())
                if (auto* prop = reflect::find_property(static_cast<ue::UStruct*>(wl->GetClassPrivate()), L"PlayerStartTag"))
                    tag = ue::utf8(*reinterpret_cast<const ue::FName*>(reinterpret_cast<const std::uint8_t*>(wl) + prop->GetOffset_Internal()));
            send_rel(id, proto::encode(proto::MapMsg{g_frame->map, tag}, g_buf));
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
    case proto::Msg::MapReady: {
        proto::MapReady m;
        if (proto::decode(msg, m)) p.map_ready = m.map;
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

void tick(std::int64_t now_us) {
    if (lifecycle::state() != lifecycle::State::Settled) return;
    for (auto& [id, p] : g_peers)
        if (p.slot && !p.stalled && now_us - p.last_input_us > kStallUs) { // 500 ms without input: release keys
            input::pad::set(p.slot, 0, 0, 0);
            p.stalled = true;
        }
    capture(now_us);
    ability::host_enable(on_launch);
    send_acts();
    send_snapshots();
}

void stop() {
    for (auto& [id, p] : g_peers)
        if (p.slot) party::leave(p.slot);
    for (auto& [k, slot] : g_parked) party::leave(slot);
    ability::host_disable();
    g_pending.clear();
    g_act_seq.clear();
    g_peers.clear();
    g_parked.clear();
    g_names.clear();
}

} // namespace

std::string describe() {
    std::string out = std::format("host: peers={} parked={} names={} epoch={} actsSent={} actsDropped={}\n", g_peers.size(), g_parked.size(), g_names.size(), g_epoch,
                                  g_acts_sent, g_acts_dropped) + ability::describe();
    for (const auto& [id, p] : g_peers)
        out += std::format("  peer {} -> P{} key={} seq={} acked={} input={} mapReady={} build=[{}]\n", id, p.slot, p.guest_key, p.seq, p.acked, p.stalled ? "stalled" : "live", p.map_ready, p.build_report);
    return out;
}

void install() { coop::set_handlers(Role::Host, {on_connected, on_closed, on_reliable, on_unreliable, tick, stop}); }

} // namespace elmp::coop::host
