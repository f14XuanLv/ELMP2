// Coop host (§7.5 host/{capture,guest,events}): every connected peer gets a remote seat (party Source::Network)
// fed by its input stream; every frame the host captures one FrameSnapshot (lily rows: P1 + seats; enemy rows:
// registry Enemy bucket keyed by the SourceSpawnPoint name) and sends each peer a delta against the snapshot
// it acknowledged. Names travel once as NameDef (reliable). Disconnect without F_CLOSE keeps the seat parked
// under the guest key for reconnection (FR-NET-2c); an explicit leave removes it.

#include "coop/host.h"

#include "coop/commands.h"
#include "coop/entity.h"
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

#include "input/pad_state.h"
#include "input/virtual_pad.h"

#include "engine/binary_checks.h"

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
};

std::map<std::uint32_t, Peer> g_peers;
std::map<std::string, std::int64_t> g_parked; // guest key → seat kept after an unexpected disconnect
proto::NameTable g_names;
std::unique_ptr<snap::Frame> g_frame = std::make_unique<snap::Frame>();
std::array<std::uint8_t, 64 * 1024> g_buf;
std::uint16_t g_epoch = 1;
std::uint16_t g_adapter_map = 0;
std::string g_trace_name;
std::vector<float> g_trace;
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
    // ADR 0006: the seat's live input drives the client copies' vanilla pipeline (locomotion, holds)
    static const auto off = binary_checks::offset("ZenithInputComponent.InputSnapshot");
    if (auto* ic = off ? s.zc_input.object(pawn) : nullptr) {
        const auto& in = *reinterpret_cast<const input::InputSnapshot*>(reinterpret_cast<const std::uint8_t*>(ic) + *off);
        row->ix = static_cast<std::int8_t>(std::clamp(std::lround(in.input_x * 100), -100l, 100l));
        row->iy = static_cast<std::int8_t>(std::clamp(std::lround(in.input_y * 100), -100l, 100l));
        row->iraw = static_cast<std::uint16_t>(in.raw & input::kCommandMask);
    }
}

// the enemy spine component is cached per enemy handle — invalidated when the handle dies
std::unordered_map<std::int32_t, std::pair<ObjHandle, ObjHandle>> g_spine; // actor index → (actor, spine comp)

template <class Row>
void pose(ue::UObject* enemy, const ObjHandle& h, Row& row, const core::schema::Script& s) {
    auto& c = g_spine[h.index];
    if (c.first != h) {
        static ObjHandle cls = ObjHandle::of(reflect::find_object(L"/Script/SpineExtensionPlugin.SpineSkeletonAnimationExComponent"));
        reflect::Frame f(s.actor_component_by_class);
        f.arg(0, cls.get());
        c = {h, f.invoke(enemy) ? ObjHandle::of(f.ret<ue::UObject*>()) : ObjHandle{}};
    }
    track0(c.second.get(), row, s);
}
} // namespace

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
            if (auto at = core::location(enemy)) row->x = at->x, row->y = at->y, row->z = at->z;
            if (spn == g_trace_name && g_trace.size() < 240) g_trace.push_back(row->y); // Dev: raw host Y per frame
            if (auto* hp = s->zc_hp.object(enemy)) {
                const auto cur = s->hp_curr.read<std::int32_t>(hp);
                row->hp = static_cast<std::uint16_t>(std::max(0, cur));
                row->mhp = static_cast<std::uint16_t>(std::max(0, s->hp_max.read<std::int32_t>(hp)));
                if (cur <= 0) row->flags |= 4; // dead only when the last sample says so (unload ≠ death)
            }
            row->type = e.class_id;
            if (auto* loco = s->zc_locomotion.object(enemy)) {
                reflect::Frame fr(s->loco_facing_right);
                if (fr.invoke(loco)) row->facing = fr.ret_bool() ? 1 : -1;
            }
            pose(enemy, e.handle, *row, *s);
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
    static ue::UClass* spirit = reflect::find_class(L"/Script/Zenith.ZenithSpirit"); // derived by the summoner's command
    if (spirit && owner->GetClassPrivate()->IsChildOf(spirit)) return r.kind = proto::RefKind::Spirit, r.id = 0, r;
    // SourceSpawnPoint is a ZenithCharacter property: other owners must not be read with it (access violation
    // 0x40000000d when a lily's spirit launched an ability)
    static ue::UClass* zc = reflect::find_class(L"/Script/Zenith.ZenithCharacter");
    if (!zc || !owner->GetClassPrivate()->IsChildOf(zc) || !g_resolved) return std::nullopt;
    if (auto* sp = g_spawn_point.object(owner)) return r.kind = proto::RefKind::Enemy, r.id = name(ue::utf8_name(sp)), r;
    if (const auto d = entities::dyn_id_of(h)) return r.kind = proto::RefKind::Dyn, r.id = d, r;
    return std::nullopt;
}

bool wants(const Peer& p, const Ref& r) {
    if (!p.slot || std::max(0, maps::slot_of_seat(p.slot)) != r.slot) return false;
    if (r.kind == proto::RefKind::Lily && r.id == p.slot) return false;
    if (r.kind == proto::RefKind::Spirit && (r.id >> 8) == p.slot) return false;
    return true;
}

std::uint32_t ref_key(const Ref& r) { return static_cast<std::uint32_t>(r.kind) << 16 | r.id; }


template <class Msg>
void send_to(const Ref& r, const Msg& m, std::uint64_t& counter) {
    const auto n = proto::encode(m, g_buf);
    if (!n) return;
    for (auto& [id, peer] : g_peers)
        if (wants(peer, r)) send_rel(id, n), ++counter;
}

// Command channel (ADR 0006): command starts of replicated actors, flushed before the snapshot of the frame.
// Spirit commands are not sent: the client's copy derives them from the summoner's replayed command.
struct CmdEv {
    ObjHandle owner, cls;
    commands::Start s;
    std::uint32_t t_ms = 0;
};
std::vector<CmdEv> g_cmd_pending;
std::unordered_map<std::uint32_t, std::uint32_t> g_cmd_seq; // ref key → last cmd seq
std::uint64_t g_cmds_sent = 0, g_cmds_dropped = 0, g_cmds_noref = 0, g_cmds_spirit = 0;

void on_cmd(const commands::Start& s) {
    if (g_peers.empty()) return;
    if (g_cmd_pending.size() >= 128) return void(++g_cmds_dropped);
    g_cmd_pending.push_back({ObjHandle::of(s.owner), ObjHandle::of(s.cls), s, static_cast<std::uint32_t>(clock::now_us() / 1000)});
}

void send_cmds() {
    for (auto& e : g_cmd_pending) {
        const auto r = e.cls.get() ? ref_of(e.owner.get()) : std::nullopt;
        if (!r) {
            ++g_cmds_noref;
            continue;
        }
        if (r->kind == proto::RefKind::Spirit) {
            ++g_cmds_spirit;
            continue;
        }
        proto::Cmd m{static_cast<std::uint8_t>(r->kind), r->id, ++g_cmd_seq[ref_key(*r)], e.t_ms, name(e.s.path), e.s.dir, e.s.cmd};
        if (!m.cls || m.cmd > proto::kCmdTypeMax || m.dir > proto::kCmdDirMax) continue;
        send_to(*r, m, g_cmds_sent);
    }
    g_cmd_pending.clear();
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
        }
        if (p.map_sent != v.map) { // follow the seat's map (client replies MapReady)
            p.map_sent = v.map;
            std::string tag = slot == 0 ? std::string{} : maps::tag_of_seat(p.slot);
            if (slot == 0)
                if (auto* wl = world::world_loader())
                    if (auto* prop = reflect::find_property(static_cast<ue::UStruct*>(wl->GetClassPrivate()), L"PlayerStartTag"))
                        tag = ue::utf8(*reinterpret_cast<const ue::FName*>(reinterpret_cast<const std::uint8_t*>(wl) + prop->GetOffset_Internal()));
            send_rel(id, proto::encode(proto::MapMsg{v.map, tag}, g_buf));
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
    commands::host_enable(on_cmd); // ADR 0006: decisions only; the P6 product channels (act / fx / sound / stop) are off
    refresh_lily_pawns();
    party::transit::set_stay([](std::int64_t seat) { return maps::slot_of_seat(seat) > 0; });
    maps::tick(now_us);
    send_cmds();
    send_snapshots();
}

void stop() {
    for (auto& [id, p] : g_peers)
        if (p.slot) party::leave(p.slot);
    for (auto& [k, slot] : g_parked) party::leave(slot);
    commands::host_disable();
    g_cmd_pending.clear(), g_cmd_seq.clear();
    party::transit::set_stay(nullptr);
    maps::reset();
    g_peers.clear();
    g_parked.clear();
    g_names.clear();
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
    std::string out = std::format("host: peers={} parked={} names={} epoch={}\n", g_peers.size(), g_parked.size(), g_names.size(), g_epoch) +
                      std::format("cmds: sent={} dropped={} noOwner={} spiritSkipped={}\n", g_cmds_sent, g_cmds_dropped, g_cmds_noref, g_cmds_spirit) + commands::describe();
    for (auto* a : adapters()) out += a->describe();
    out += maps::describe();
    for (const auto& [id, p] : g_peers)
        out += std::format("  peer {} -> P{} key={} seq={} acked={} input={} mapReady={} build=[{}]\n", id, p.slot, p.guest_key, p.seq, p.acked, p.stalled ? "stalled" : "live", p.map_ready, p.build_report);
    return out;
}

void install() { coop::set_handlers(Role::Host, {on_connected, on_closed, on_reliable, on_unreliable, tick, stop}); }

} // namespace elmp::coop::host
