// Coop client (first slice): guest key → Build; Welcome arms the guard; every frame the local P1 input
// snapshot goes upstream (axes, held bits, cumulative press counters, snapshot ack); snapshots are decoded
// against the acknowledged base; enemies named by their spawn point are taken over (guard mark on pawn +
// controller, actor tick off, CanBePaused untouched by vanilla since activation is gated) and placed /
// HP-written from the host rows. The client process never settles (guard), never saves (guard).

#include "coop/client.h"
#include "coop/native_lily.h"
#include "coop/autonomous.h"
#include "coop/lily_events.h"
#include "coop/causality.h"

#include "coop/ability.h"
#include "coop/reaction.h"
#include "coop/presentation.h"
#include "coop/presentation_policy.h"
#include "coop/visual_state.h"
#include "coop/fx.h"
#include "overlay/render.h"
#include "coop/entities/clearable.h"
#include "coop/entities/companion.h"
#include "coop/entities/spawn.h"
#include "coop/entity.h"
#include "coop/session.h"

#include "guard/guard.h"

#include "core/player.h"
#include "core/permission.h"
#include "core/profile.h"
#include "core/schema.h"
#include "core/skin.h"
#include "core/spawn.h"

#include "engine/binary_checks.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "input/pad_state.h"
#include "party/ui.h"

#include "net/protocol.h"
#include "net/snapshot.h"

#include "base/clock.h"
#include "base/json.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/settings.h"

#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

namespace elmp::coop::client {

// P6: replicated owner (net::proto::RefKind) → the local presented actor, nullptr when not presented
ue::UObject* actor_of(std::uint8_t kind, std::uint16_t id, bool allow_dying = false, bool feedback = false);

namespace {

namespace snap = net::snap;
namespace proto = net::proto;

constexpr std::size_t kRing = 32;
constexpr float kReconcileUU = 150.0f; // local P1 vs host seat drift before a snap (decision P2: prediction + snap)
std::uint64_t g_reconciles = 0;
std::uint32_t g_reaction_last = 0, g_reaction_time = 0;
std::array<std::uint32_t, 9> g_reaction_seats{};
bool g_reaction_anchor = false;
std::uint64_t g_reaction_received = 0, g_reaction_stale = 0, g_reaction_failed = 0;
std::string g_reaction_failure;

void note_host_time(std::uint32_t host_ms, std::int64_t now_us);
void reset_dynamic();

struct Sample {
    std::uint32_t t = 0; // host ms
    float x = 0, y = 0, z = 0;
};
struct Taken {
    std::uint32_t instance = 0;
    std::array<Sample, 16> s{}; // ≥ 100 ms buffer at 60 Hz host capture (4 was too few: held the oldest sample, stepped on rotation)
    int head = 0, n = 0;
    ObjHandle enemy, controller;
    bool ready = false;
    bool killed = false, hidden = false;
    bool engaged = false;    // 当前呈现的副本是否已开 BattleColor（红光）
    std::int8_t facing = 0;
    std::uint32_t frame = 0; // last snapshot that carried the row
    std::int32_t hp = -1;    // last presented HP (hit reaction on a drop)
};

std::uint8_t g_seat = 0;
bool g_seat_placed = false;
bool g_refresh_pending = false;
std::uint64_t g_refreshes = 0;
std::string g_last_refresh;
std::uint16_t g_epoch = 0, g_host_map = 0;
std::uint32_t g_latest = 0;
std::uint32_t g_host_view = 0;
ue::UObject* causal_target(std::uint8_t kind, std::uint16_t id, std::uint32_t generation);
void causal_motion(double host_ms);
void causal_api();
std::string g_follow_tag;
std::string g_equip_sent, g_equip_result;
std::uint64_t g_equip_requests = 0;
bool g_follow_pending = false;
std::int64_t g_follow_next_us = 0;
std::unique_ptr<std::array<snap::Frame, kRing>> g_ring;
proto::NameTable g_names;
void causal_api() { causality::client_api({[](std::uint16_t id){return g_names.text(id);},causal_target,causal_motion}); }
proto::Input g_input;
std::array<std::uint8_t, 2048> g_buf;
std::unordered_map<std::string, Taken> g_taken; // spawn point name → local enemy
std::uint64_t g_index_version = ~0ull;
std::unordered_map<std::string, ObjHandle> g_index; // spawn point name → enemy (rebuilt on registry change)
reflect::PropRef g_spawn_point;
reflect::FuncRef g_set_tick, g_set_collision, g_set_rotation;
bool g_resolved = false;
std::uint64_t g_unknown_names = 0, g_bad_snaps = 0, g_applied = 0, g_kills = 0, g_act_bad = 0, g_act_absent = 0, g_rechecks = 0, g_leaks = 0, g_pawn_activated = 0;
std::unordered_map<std::uint32_t, std::uint32_t> g_act_last; // kind << 16 | owner id → last act seq
metrics::Histogram& h_apply = metrics::histogram("coop.client.apply_us");
metrics::Histogram& h_motion = metrics::histogram("coop.client.motion_us");
metrics::Histogram& h_replay = metrics::histogram("coop.client.replay_us");
metrics::Histogram& h_write = metrics::histogram("coop.client.motion_write_us");
metrics::Histogram& h_input = metrics::histogram("coop.client.input_us");
metrics::Histogram& h_slow = metrics::histogram("coop.client.slow_us"); // 1 Hz: equipment poll + recheck

std::string guest_key() {
    const auto dir = std::filesystem::path(world::saved_dir()) / L"ELMP" / L"ELMP";
    const auto file = dir / L"guest.json";
    std::ifstream in(file);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    if (const auto k = text.find("\"guestKey\":\""); k != std::string::npos) {
        const auto e = text.find('"', k + 12);
        if (e != std::string::npos && e - k - 12 == 32) return text.substr(k + 12, 32);
    }
    std::array<std::uint8_t, 16> r{};
    net::crypto::random(r);
    const auto key = net::to_hex(r);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(file) << std::format("{{\"schema\":1,\"guestKey\":\"{}\"}}\n", key);
    return key;
}

void resolve() {
    if (g_resolved) return;
    reflect::Resolver r("ZenithCharacter", reflect::find_class(L"/Script/Zenith.ZenithCharacter"));
    g_spawn_point = r.prop(L"SourceSpawnPoint", reflect::Kind::Object);
    reflect::Resolver a("Actor", reflect::find_class(L"/Script/Engine.Actor"));
    g_set_tick = a.func(L"SetActorTickEnabled", 1);
    g_resolved = r.ok() && a.ok();
}

void reindex() {
    const auto v = registry::version(registry::Kind::Enemy) ^ (registry::size(registry::Kind::Enemy) << 40);
    if (v == g_index_version) return;
    g_index_version = v;
    g_index.clear();
    for (const auto& e : registry::bucket(registry::Kind::Enemy))
        if (auto* enemy = e.handle.get())
            if (auto* sp = g_spawn_point.object(enemy)) g_index[ue::utf8_name(sp)] = e.handle;
}

void send_input() {
    g_input.x = g_input.y = 0; g_input.raw = 0;
    const auto* s = core::schema::script();
    auto* p1 = world::p1_pawn();
    const auto off = binary_checks::offset("ZenithInputComponent.InputSnapshot");
    auto* ic = s && p1 ? s->zc_input.object(p1) : nullptr;
    if (ic && off && !party::ui::gameplay_blocked()) {
        const auto& snapshot = *reinterpret_cast<const input::InputSnapshot*>(reinterpret_cast<const std::uint8_t*>(ic) + *off);
        g_input.x = static_cast<std::int8_t>(std::clamp(std::lround(snapshot.input_x * 100), -100l, 100l));
        g_input.y = static_cast<std::int8_t>(std::clamp(std::lround(snapshot.input_y * 100), -100l, 100l));
        g_input.raw = static_cast<std::uint16_t>(snapshot.raw & input::kCommandMask);
        for (int c = 1; c < 10; ++c)
            if (snapshot.flags & (1u << c)) ++g_input.presses[c];
    }
    g_input.epoch = g_epoch;
    g_input.snap_ack = g_latest;
    if (const auto n = proto::encode(g_input, g_buf)) coop::send_unreliable(0, proto::kInputStream, std::span(g_buf.data(), n));
}

// One policy for all remote actors. A failed root-motion boundary stays hidden/frozen and is retried.
void release_presentation(ue::UObject* actor) {
    if (!actor) return;
    presentation::forget(actor);
    visual_state::forget(actor);
    presentation_policy::release(actor);
    if (const auto* s = core::schema::script()) {
        static auto* pawn_class = reflect::find_class(L"/Script/Engine.Pawn");
        if (pawn_class && actor->IsA(pawn_class)) guard::unmark(s->pawn_controller.object(actor));
    }
    guard::unmark(actor);
}

bool take(const std::string& name, ue::UObject* enemy) {
    auto* controller = core::schema::script() ? core::schema::script()->pawn_controller.object(enemy) : nullptr;
    Taken t;
    t.enemy = ObjHandle::of(enemy);
    t.controller = ObjHandle::of(controller);
    t.ready = presentation_policy::apply(enemy);
    g_taken[name] = t;
    if (!t.ready) entities::hide_actor(enemy);
    return t.ready;
}

void actor_hidden(ue::UObject* actor, bool hidden) {
    static reflect::FuncRef f = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"SetActorHiddenInGame", 1);
    if (actor && f.ok()) reflect::Frame(f).arg_bool(0, hidden).invoke(actor);
}

void present_lilies(const snap::Frame& f, const core::schema::Script& s);

// One enemy row (adapter enemy; also used by enemy_dyn for adopted / spawned copies): takeover on first
// sight, facing, buffered position, HP, Spine pose.
void present_enemy(const std::string& key, ue::UObject* enemy, const snap::EnemyRow& row, const snap::Frame& f, const core::schema::Script& s_) {
    const auto* s = &s_;
    if (auto t = g_taken.find(key); t == g_taken.end() || t->second.enemy.get() != enemy) take(key, enemy);
    auto& t = g_taken[key];
    t.instance = row.instance;
    if (!t.ready) {
        t.ready = presentation_policy::apply(enemy);
        if (!t.ready) return;
        actor_hidden(enemy, (row.flags & 2) != 0);
    }
    if (t.hidden != ((row.flags & 2) != 0)) actor_hidden(enemy, (row.flags & 2) != 0);
    t.hidden = (row.flags & 2) != 0;
    if (row.facing && row.facing != t.facing) // vanilla facing entry (v1 net-facts: LocomotionComponent:SetFacing)
        if (auto* loco = s->zc_locomotion.object(enemy)) {
            static reflect::FuncRef set_facing =
                reflect::Resolver("LocomotionComponent", reflect::find_class(L"/Script/Zenith.LocomotionComponent")).func(L"SetFacing", 1);
            if (set_facing.ok()) {
                reflect::Frame ff(set_facing);
                ff.arg_bool(0, row.facing > 0);
                if (ff.invoke(loco)) t.facing = row.facing;
            }
        }
    // position: buffered by host time, presented every frame with interpolation (present_motion)
    if (!t.n || f.t_ms != t.s[(t.head + t.n - 1) % 16].t) {
        const int at = (t.head + t.n) % 16;
        t.s[at] = {f.t_ms, row.x, row.y, row.z};
        if (t.n < 16) ++t.n;
        else t.head = (t.head + 1) % 16;
    }
    if (auto* hp = s->zc_hp.object(enemy)) {
        // property write (no SetCurrHP in vanilla). Death is presentation only: the host's death pose keeps
        // streaming while the row exists; when the host removes the actor the copy is hidden (below). The
        // vanilla death pipeline is never run here (it would clear the client's spawn point — v1 net-facts
        // 「重构后联机验收」defect 1).
        *s->hp_curr.at<std::int32_t>(hp) = row.hp;
        t.hp = row.hp;
        const bool killed = (row.flags & 4) != 0;
        if (killed && !t.killed) ++g_kills;
        t.killed = killed;
    }
    t.frame = g_latest;
    if (const bool engaged = (row.flags & 8) != 0; engaged != t.engaged)
        if (presentation_policy::engage(enemy, engaged)) t.engaged = engaged;
    visual_state::apply(enemy, row.visual);
    presentation::apply(enemy, row.pose, [](std::uint16_t id) { return g_names.text(id); });
}

void present_enemies_impl(const snap::Frame& f, const core::schema::Script& s_) {
    const auto* s = &s_;
    for (const auto& row : f.e.view()) {
        const auto* name = g_names.text(row.key);
        if (!name) {
            ++g_unknown_names;
            continue;
        }
        auto it = g_index.find(*name);
        auto* enemy = it == g_index.end() ? nullptr : it->second.get();
        if (!enemy) continue; // not streamed in locally (Client ⊆ Host)
        present_enemy(*name, enemy, row, f, *s);
    }
    // host-dead enemies whose row is gone: the host destroyed them → hide + no collision (residency: a row
    // absent without a death is an unload / interest change and stays as it was)
    static reflect::FuncRef hide;
    if (!hide.ok()) hide = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"SetActorHiddenInGame", 1);
    for (auto& [n, t] : g_taken)
        if (t.killed && !t.hidden && t.frame != g_latest)
            if (auto* e = t.enemy.get(); e && hide.ok()) {
                reflect::Frame h(hide);
                h.arg_bool(0, true);
                h.invoke(e);
                if (g_set_collision.ok()) {
                    reflect::Frame fc(g_set_collision);
                    fc.arg_bool(0, false);
                    fc.invoke(e);
                }
                t.hidden = true;
            }
}

void apply(const snap::Frame& f) {
    const auto t0 = clock::now_us();
    const auto* s = core::schema::script();
    resolve();
    if (!s || !g_resolved) return;
    const auto* map = g_names.text(f.map);
    if (f.map != g_host_map || !map || *map != ue::utf8(world::current_map_id())) return;
    reindex();
    ClientCtx ctx{f, [](std::uint16_t id) { return g_names.text(id); }};
    for (auto* a : adapters()) a->apply_client(ctx);
    ++g_applied;
    h_apply.add(static_cast<double>(clock::now_us() - t0));
}

// Copies of the other lilies on the host (host P1 + other seats): a pooled ELMP Lily at slot 50 + key,
// marked (no AI / no collision / IsPlayer false), positioned / turned / animated from the l rows.
struct Copy {
    std::int64_t slot = 0;
    std::uint16_t skin = 0;
    std::int8_t facing = 0;
    bool seen = false, ready = false;
};
std::map<std::uint16_t, Copy> g_copies;

void drop_copy(std::uint16_t key) {
    auto it = g_copies.find(key);
    if (it == g_copies.end()) return;
    if (auto* p = core::pawn(it->second.slot)) release_presentation(p), guard::mark_not_player(p, false);
    core::spawn::despawn(it->second.slot, false);
    core::leases().release({"ELMP", slots::OwnerKind::Elmp}, it->second.slot);
    native_lily::forget(key);
    g_copies.erase(it);
}

void drop_copies() {
    while (!g_copies.empty()) drop_copy(g_copies.begin()->first);
}

void present_lilies(const snap::Frame& f, const core::schema::Script& s) {
    if (!g_set_collision.ok()) {
        reflect::Resolver a("Actor", reflect::find_class(L"/Script/Engine.Actor"));
        g_set_collision = a.func(L"SetActorEnableCollision", 1);
        g_set_rotation = a.func(L"K2_SetActorRotation", 2);
    }
    for (auto& [k, c] : g_copies) c.seen = false;
    for (const auto& row : f.l.view()) {
        if (row.key == g_seat) { // own seat: the local P1 predicts; snap on first placement or when it drifts from the host seat
            auto* p1 = world::p1_pawn();
            if (auto at = p1 ? core::location(p1) : std::nullopt;
                at && (!g_reaction_anchor || static_cast<std::int32_t>(f.t_ms - g_reaction_time) >= 0) &&
                (!g_seat_placed || std::hypot(at->y - row.y, at->z - row.z) > kReconcileUU) &&
                (!g_reaction_last || !reaction::active(p1))) {
                reflect::Frame tp(s.actor_teleport);
                tp.arg(0, core::Vec3{row.x, row.y, row.z}).arg_bool(1, false).arg_bool(3, true);
                if (tp.invoke(p1)) g_seat_placed = true, ++g_reconciles;
            }
            if (auto* hp = p1 ? s.zc_hp.object(p1) : nullptr) { // own vitals are the host seat's (HUD shows them)
                *s.hp_max.at<std::int32_t>(hp) = row.mhp;
                *s.hp_curr.at<std::int32_t>(hp) = std::max<std::int32_t>(row.hp, row.alive ? 1 : 0);
            }
            if(p1&&!lily_events::apply(p1,row.events,f.t_ms,[](std::uint16_t id){return g_names.text(id);}))return coop::leave("Lily external animation context unavailable");
            continue;
        }
        if (row.key == 0 || row.key > 8) continue;
        auto& c = g_copies[row.key];
        c.seen = true;
        if (!c.slot || !core::pawn(c.slot)) {
            const std::int64_t slot = 50 + row.key;
            if (!c.slot && core::leases().acquire({"ELMP", slots::OwnerKind::Elmp}, slot) != slots::Result::Ok) continue;
            c.slot = slot;
            core::spawn::Options o;
            o.location = core::Vec3{row.x, row.y, row.z};
            o.skin = row.skin ? row.skin : 1;
            if (const auto why = core::spawn::spawn(slot, o); !why.empty()) { log::warn("native Lily spawn: {}", why); continue; }
            c.skin = o.skin;
            if (const auto why = native_lily::bind(row.key, slot, core::pawn(slot)); !why.empty()) { coop::leave(why); return; }
            c.ready = true;
        }
        auto* p = core::pawn(c.slot);
        if (!p) continue;
        if (core::skin::valid(row.skin) && row.skin != c.skin) {
            if (const auto why = core::skin::apply(p, row.skin); !why.empty()) { coop::leave(why); return; }
            c.skin = row.skin;
        }
        // Vitals are Host state; all animation, summon and movement writers belong to vanilla.
        if (auto* hp = s.zc_hp.object(p)) {
            *s.hp_max.at<std::int32_t>(hp) = row.mhp;
            *s.hp_curr.at<std::int32_t>(hp) = std::max<std::int32_t>(row.hp, 1);
        }
        if (auto* mp = s.zc_mp.object(p)) {
            *s.mp_max.at<std::int32_t>(mp) = row.mmp;
            *s.mp_curr.at<std::int32_t>(mp) = row.mp;
        }
        if(!lily_events::apply(p,row.events,f.t_ms,[](std::uint16_t id){return g_names.text(id);}))return coop::leave("Lily external animation context unavailable");
    }
    std::vector<std::uint16_t> gone;
    for (auto& [k, c] : g_copies)
        if (!c.seen) gone.push_back(k);
    for (auto k : gone) drop_copy(k);
}

// P6 event channel: acts / FX / FX stops / sounds / ability stops of any replicated owner. An event whose owner
// is not presented yet (e.g. a spirit's first ability arriving before the snapshot that summons it) is retried
// for up to 300 ms, in order.
constexpr std::int64_t kRetryUs = 300'000;
constexpr float kSoundRangeUU = 4000.0f;
enum class EvResult { Done, Retry };
struct RetryEv {
    std::int64_t deadline = 0;
    std::vector<std::uint8_t> msg;
};
std::deque<RetryEv> g_ev_retry;
struct PresentedAbility {
    ObjHandle actor;
    std::uint64_t activation = 0;
};
std::unordered_map<std::uint64_t, PresentedAbility> g_ab_live; // wire activation → exact native activation
struct PresentedFx {
    ObjHandle psc, owner;
};
// A wire activation may survive a local owner rebuild. Weak serials plus the actual owner binding
// prevent its periodic baseline from being mistaken for a still-present component on the old actor.
std::unordered_map<std::uint32_t, PresentedFx> g_fx_live;
std::uint64_t g_ev_absent = 0, g_ev_retried = 0, g_snd_far = 0, g_fx_bad = 0, g_snd_bad = 0, g_stop_missed = 0;
std::uint64_t g_fx_kept = 0, g_fx_replaced = 0;
std::uint64_t g_child_stopped = 0, g_child_ended = 0, g_child_failed = 0;
std::string g_event_failure;

bool feedback_message(std::span<const std::uint8_t> bytes) {
    if (proto::type_of(bytes) == proto::Msg::Fx) {
        proto::Fx m;
        return proto::decode(bytes, m) && (m.flags & 32);
    }
    if (proto::type_of(bytes) == proto::Msg::Snd) {
        proto::Snd m;
        return proto::decode(bytes, m) && (m.flags & 1);
    }
    return false;
}

bool world_ready();
std::string feedback_pending(std::span<const std::uint8_t> bytes) {
    proto::Fx fx; proto::Snd sound;
    const bool particle = proto::type_of(bytes) == proto::Msg::Fx;
    if (particle ? !proto::decode(bytes,fx) : !proto::decode(bytes,sound)) return "invalid message";
    const auto kind=particle?fx.kind:sound.kind;const auto id=particle?fx.net_id:sound.net_id;
    const auto root=particle?fx.causal_root:sound.causal_root;const auto time=particle?fx.t_ms:sound.t_ms;
    const auto asset=particle?fx.ps:sound.event;const auto* path=g_names.text(asset);
    return std::format("{} ref={}:{} asset={} '{}' root={} host_ms={} playback={:.3f} due={} owner={} worldReady={} {}",particle?"FX":"sound",kind,id,asset,path?*path:"<undefined>",root,time,causality::playback_ms(),!root||causality::event_due(root,time),actor_of(kind,id,true,true)!=nullptr,world_ready(),causality::pending_root(root));
}

void clear_events() {
    g_ev_retry.clear();
    for (auto& [id, e] : g_fx_live) fx::retire(e.psc.get());
    ability::client_reset(); // Stops every network-owned native root/child while settlement gates remain armed.
    g_fx_live.clear();
    g_ab_live.clear();
    g_act_last.clear();
    g_event_failure.clear();
}

void reset_remote_world() {
    // End replayed results while their owners are still marked and settlement/FX gates are armed.
    // This is a map/session boundary, not a MapReady acknowledgement or a follow retry.
    clear_events();
    drop_copies();
    ability::client_reset(); // original equipment teardown may have emitted a final activation
    native_lily::reset();
    lily_events::reset();
    reset_dynamic();
    for (auto& [name, t] : g_taken) release_presentation(t.enemy.get());
    g_taken.clear();
    for (auto* a : adapters()) a->reset();
    presentation_policy::reset();
    presentation::reset();
    visual_state::reset();
    g_index.clear();
    g_index_version = ~0ull;
    g_seat_placed = false;
    g_reaction_anchor = false;
}

bool world_ready() {
    bool loading = true, ready = false;
    const auto* map = g_names.text(g_host_map);
    return map && *map == ue::utf8(world::current_map_id()) && lifecycle::state() == lifecycle::State::Settled &&
           world::loader_flags(loading, ready) && !loading && ready;
}

std::uint64_t ab_key(std::uint8_t kind, std::uint16_t id, std::uint32_t seq) { return static_cast<std::uint64_t>(kind) << 48 | static_cast<std::uint64_t>(id) << 32 | seq; }

EvResult handle_event(std::span<const std::uint8_t> msg) {
    switch (proto::type_of(msg)) {
    case proto::Msg::Reaction: {
        if (!g_reaction_failure.empty()) return EvResult::Done;
        proto::Reaction m;
        if (!proto::decode(msg, m)) {
            ++g_reaction_failed;
            g_reaction_failure = "invalid authoritative reaction";
            return EvResult::Done;
        }
        if (!m.seat || m.seat > 8 || m.epoch != g_epoch || m.map != g_host_map || m.seq <= g_reaction_seats[m.seat])
            return ++g_reaction_stale, EvResult::Done;
        if (!world_ready()) return EvResult::Retry;
        if(m.seat!=g_seat&&causality::playback_ms()>0&&m.t_ms>causality::playback_ms())return EvResult::Retry;
        if ((m.curve && !g_names.text(m.curve))) return EvResult::Retry;
        for (std::size_t i = 0; i < m.state_count; ++i)
            if (!g_names.text(m.states[i])) return EvResult::Retry;
        auto* p1 = m.seat == g_seat ? world::p1_pawn() : native_lily::actor(m.seat);
        if (!p1) return EvResult::Retry;
        std::string why;
        g_reaction_seats[m.seat] = m.seq;
        if (m.seat == g_seat) g_reaction_last = m.seq;
        if (m.kind == 0 && m.seat == g_seat) g_reaction_time = m.t_ms, g_reaction_anchor = true;
        if (!reaction::replay(p1, m, [](std::uint16_t id) { return g_names.text(id); }, &why)) {
            ++g_reaction_failed;
            g_reaction_failure = "authoritative reaction context: " + why;
            log::warn("coop: {}", g_reaction_failure);
        } else {
            if (m.kind == 0 && m.seat == g_seat) g_seat_placed = true;
            ++g_reaction_received;
        }
        return EvResult::Done;
    }
    case proto::Msg::Act: {
        proto::Act a;
        if (!proto::decode(msg, a)) return ++g_act_bad, EvResult::Done;
        auto& last = g_act_last[static_cast<std::uint32_t>(a.kind) << 16 | a.net_id];
        if (a.seq <= last) return EvResult::Done; // idempotent per owner
        // An owner snapshot may arrive between two reliable Acts. Preserve causal order across
        // retries as well, or the newer last-sequence would silently discard the older family.
        for (const auto& pending : g_ev_retry) {
            proto::Act older;
            if (proto::type_of(pending.msg) == proto::Msg::Act && proto::decode(pending.msg, older) &&
                older.kind == a.kind && older.net_id == a.net_id && older.seq < a.seq) return EvResult::Retry;
        }
        const auto* cls = g_names.text(a.ability);
        const auto* cs = a.custom_spawn ? g_names.text(a.custom_spawn) : nullptr;
        const auto* ca = a.custom_attach ? g_names.text(a.custom_attach) : nullptr;
        if (!cls) return ++g_act_bad, EvResult::Done;
        auto* owner = actor_of(a.kind, a.net_id);
        if (!owner) return EvResult::Retry;
        last = a.seq;
        std::string why;
        ue::UObject* spawned = nullptr;
        std::uint64_t activation = 0;
        const auto r0 = clock::now_us();
        const bool ok = ability::replay(owner, a, *cls, cs ? *cs : "", ca ? *ca : "", &why, &spawned, &activation);
        h_replay.add(static_cast<double>(clock::now_us() - r0));
        if (!ok && ++g_act_bad <= 5) log::warn("coop: act {} on kind {} id {} rejected: {}", *cls, a.kind, a.net_id, why);
        if (spawned) {
            const auto h = ObjHandle::of(spawned);
            // ReusableActor can return the same UObject for a new launch. A stale ActStop
            // must never retain a binding to the new activation of that pooled object.
            std::erase_if(g_ab_live, [&](const auto& old) {
                return old.second.actor == h && !ability::has_active_family(old.second.activation);
            });
            if (activation) g_ab_live[ab_key(a.kind, a.net_id, a.seq)] = {h, activation};
        }
        if (g_ab_live.size() > 512) std::erase_if(g_ab_live, [](const auto& kv) {
            return !ability::has_active_family(kv.second.activation);
        });
        return EvResult::Done;
    }
    case proto::Msg::ChildStop: {
        if (!g_event_failure.empty()) return EvResult::Done;
        proto::ChildStop m;
        if (!proto::decode(msg, m)) {
            ++g_child_failed;
            g_event_failure = "invalid authoritative child stop";
            return EvResult::Done;
        }
        const auto root = g_ab_live.find(ab_key(m.kind, m.net_id, m.seq));
        if (root == g_ab_live.end()) {
            // Reliable order puts the root Act first, but its owner may still be waiting for a
            // snapshot. Do not lose its child completion while that exact root remains queued.
            for (const auto& pending : g_ev_retry) {
                proto::Act act;
                if (proto::type_of(pending.msg) == proto::Msg::Act && proto::decode(pending.msg, act) &&
                    act.kind == m.kind && act.net_id == m.net_id && act.seq == m.seq) return EvResult::Retry;
            }
            return ++g_child_ended, EvResult::Done; // retired root/map; no local family can still execute
        }
        const auto* cls = g_names.text(m.ability);
        if (!cls) return EvResult::Retry;
        const auto token = root->second.activation;
        switch (ability::child_state(token, m.path, *cls)) {
        case ability::ChildState::Ended: return ++g_child_ended, EvResult::Done;
        case ability::ChildState::Missing:
            return ability::has_active_family(token) ? EvResult::Retry : (++g_child_ended, EvResult::Done);
        case ability::ChildState::Conflict:
            ++g_child_failed;
            g_event_failure = "authoritative child identity conflicts with native timeline";
            return EvResult::Done;
        case ability::ChildState::Active: break;
        }
        std::uint64_t activation = 0;
        auto* child = ability::find_child(token, m.path, *cls, &activation);
        if (!child || !activation || ability::activation(child) != activation) return EvResult::Retry;
        if (fx::stop_ability(child, m.reason)) ++g_child_stopped;
        else if (ability::child_state(token, m.path, *cls) == ability::ChildState::Ended) ++g_child_ended;
        else {
            ++g_child_failed;
            g_event_failure = "authoritative child stop could not execute native Stop";
        }
        return EvResult::Done;
    }
    case proto::Msg::Fx: {
        proto::Fx m;
        if (!proto::decode(msg, m)) {
            g_event_failure = "invalid authoritative FX message";
            return ++g_fx_bad, EvResult::Done;
        }
        const bool feedback = (m.flags & 32) != 0;
        if(m.causal_root&&!causality::event_due(m.causal_root,m.t_ms))return EvResult::Retry;
        if (feedback && !world_ready()) return EvResult::Retry;
        auto* owner = actor_of(m.kind, m.net_id, true, feedback);
        const auto owner_handle = ObjHandle::of(owner);
        if (auto it = g_fx_live.find(m.seq); it != g_fx_live.end()) {
            auto* psc = it->second.psc.get();
            // Completed one-shots stay idempotent while their PSC lives. Persistent baselines instead
            // restore a missing/inactive PSC or one attached to an older incarnation of the owner.
            if (owner && it->second.owner == owner_handle && psc && (!(m.flags & 4) || fx::active(psc)))
                return ++g_fx_kept, EvResult::Done;
            fx::retire(psc);
            g_fx_live.erase(it);
            ++g_fx_replaced;
        }
        if (!owner) return EvResult::Retry;
        const auto* ps = g_names.text(m.ps);
        const auto* bone = m.custom_bone ? g_names.text(m.custom_bone) : nullptr;
        const auto* attach = m.attach_component ? g_names.text(m.attach_component) : nullptr;
        const auto* socket = m.attach_socket ? g_names.text(m.attach_socket) : nullptr;
        if ((m.custom_bone && !bone) || (m.attach_component && !attach) || (m.attach_socket && !socket)) return EvResult::Retry;
        if (!ps) return feedback ? EvResult::Retry : (++g_fx_bad, EvResult::Done);
        std::string why;
        if (auto* psc = (m.flags & 8) ? fx::replay_emitter(owner, m, *ps, attach ? *attach : "", socket ? *socket : "", &why)
                                      : fx::replay_spawn(owner, m, *ps, bone ? *bone : "", &why); ue::alive(psc)) {
            const auto handle = ObjHandle::of(psc);
            // PSC pooling preserves the UObject serial across activations. The newest wire ID owns
            // this activation; a delayed stop for its predecessor must no longer find this component.
            std::erase_if(g_fx_live, [&](const auto& e) { return e.second.psc == handle; });
            g_fx_live[m.seq] = {handle, owner_handle};
            causality::presented(m.causal_root, m.t_ms, false);
        } else {
            if (feedback) g_event_failure = "authoritative feedback FX failed: " + why;
            if (++g_fx_bad <= 5) log::warn("coop: fx {} rejected: {}", *ps, why);
        }
        if (g_fx_live.size() > 512) std::erase_if(g_fx_live, [](const auto& kv) {
            if (kv.second.owner.get() && kv.second.psc.get()) return false;
            fx::retire(kv.second.psc.get());
            return true;
        });
        return EvResult::Done;
    }
    case proto::Msg::FxStop: {
        proto::FxStop m;
        if (!proto::decode(msg, m)) return EvResult::Done;
        const bool pending_spawn=std::any_of(g_ev_retry.begin(),g_ev_retry.end(),[&](const RetryEv& e){proto::Fx pending;return proto::type_of(e.msg)==proto::Msg::Fx&&proto::decode(e.msg,pending)&&pending.seq==m.seq;});
        if((g_fx_live.contains(m.seq)||pending_spawn)&&causality::playback_ms()>0&&m.t_ms>causality::playback_ms())return EvResult::Retry;
        // Stop also cancels a spawn still waiting for its owner snapshot.
        std::erase_if(g_ev_retry, [&](const RetryEv& e) {
            proto::Fx pending;
            return proto::type_of(e.msg) == proto::Msg::Fx && proto::decode(e.msg, pending) && pending.seq == m.seq;
        });
        auto it = g_fx_live.find(m.seq);
        if (it == g_fx_live.end()) return EvResult::Done; // never replayed here (other map / own seat / dropped)
        fx::retire(it->second.psc.get());
        g_fx_live.erase(it);
        return EvResult::Done;
    }
    case proto::Msg::Snd: {
        proto::Snd m;
        if (!proto::decode(msg, m)) {
            g_event_failure = "invalid authoritative sound message";
            return ++g_snd_bad, EvResult::Done;
        }
        const bool feedback = (m.flags & 1) != 0;
        if(m.causal_root&&!causality::event_due(m.causal_root,m.t_ms))return EvResult::Retry;
        if (feedback && !world_ready()) return EvResult::Retry;
        const auto* ev = g_names.text(m.event);
        auto* owner = actor_of(m.kind, m.net_id, true, feedback);
        if (!ev) return feedback ? EvResult::Retry : (++g_snd_bad, EvResult::Done);
        if (!owner) return EvResult::Retry;
        const auto me = core::location(world::p1_pawn());
        const auto at = (m.flags & 2) && !m.is2d ? std::optional<core::Vec3>{{m.xf[4], m.xf[5], m.xf[6]}} : core::location(owner);
        if (me && at && std::fabs(me->y - at->y) + std::fabs(me->z - at->z) > kSoundRangeUU) return ++g_snd_far, EvResult::Done;
        std::string why;
        if (!fx::play_sound(owner, *ev, m, &why)) {
            if (feedback) g_event_failure = "authoritative feedback sound failed: " + why;
            if (++g_snd_bad <= 5) log::warn("coop: sound {} rejected: {}", *ev, why);
        } else causality::presented(m.causal_root, m.t_ms, true);
        return EvResult::Done;
    }
    case proto::Msg::ActStop: {
        proto::ActStop m;
        if (!proto::decode(msg, m)) return EvResult::Done;
        auto it = g_ab_live.find(ab_key(m.kind, m.net_id, m.seq));
        if (it == g_ab_live.end()) {
            // the act may still wait in the retry queue (owner not presented yet)
            const auto last = g_act_last[static_cast<std::uint32_t>(m.kind) << 16 | m.net_id];
            if (last < m.seq) return EvResult::Retry;
            return ++g_stop_missed, EvResult::Done;
        }
        if (auto* actor = it->second.actor.get(); actor && ability::activation(actor) == it->second.activation)
            fx::stop_ability(actor, m.reason);
        else ++g_stop_missed;
        // A FireAndForget child can outlive its root container (which itself may be pooled again).
        // Keep the wire root → native root token until the complete original family has ended.
        if (!ability::has_active_family(it->second.activation)) g_ab_live.erase(it);
        return EvResult::Done;
    }
    default: return EvResult::Done;
    }
}

void retry_events(std::int64_t now_us) {
    bool reaction_waiting = false;
    for (std::size_t n = g_ev_retry.size(); n && !g_ev_retry.empty(); --n) {
        auto e = std::move(g_ev_retry.front());
        g_ev_retry.pop_front();
        const bool is_reaction = proto::type_of(e.msg) == proto::Msg::Reaction;
        if (is_reaction && reaction_waiting) {
            g_ev_retry.push_back(std::move(e));
            continue;
        }
        // A deadline bounds an unresolved dependency, not a message that is ready now.
        // Resolve every event after a long frame, including feedback and FX stops.
        if (handle_event(e.msg) == EvResult::Done) {
            ++g_ev_retried;
            continue;
        }
        std::uint32_t event_time = 0;
        if (proto::type_of(e.msg) == proto::Msg::FxStop) {
            proto::FxStop stop;
            if (proto::decode(e.msg, stop)) event_time = stop.t_ms;
        } else if (feedback_message(e.msg)) {
            proto::Fx fx; proto::Snd sound;
            if (proto::decode(e.msg, fx) && fx.causal_root) event_time = fx.t_ms;
            if (proto::decode(e.msg, sound) && sound.causal_root) event_time = sound.t_ms;
        }
        if (event_time && causality::playback_ms() > 0 && event_time > causality::playback_ms())
            e.deadline = now_us + kRetryUs;
        if (now_us > e.deadline) {
            ++g_ev_absent;
            if (is_reaction) {
                ++g_reaction_failed, g_reaction_failure = "authoritative reaction context timed out";
                reaction_waiting = true;
            }
            if (proto::type_of(e.msg) == proto::Msg::ChildStop)
                ++g_child_failed, g_event_failure = "authoritative child stop context timed out";
            if (proto::type_of(e.msg) == proto::Msg::Act)
                g_event_failure = "authoritative ability launch context timed out";
            if (feedback_message(e.msg)) g_event_failure = "authoritative feedback context timed out: " + feedback_pending(e.msg);
            if (proto::type_of(e.msg) == proto::Msg::FxStop) {
                proto::FxStop stop;proto::decode(e.msg,stop);
                g_event_failure=std::format("authoritative FX stop context timed out: seq={} host_ms={} playback={:.3f} live={}",stop.seq,stop.t_ms,causality::playback_ms(),g_fx_live.contains(stop.seq));
            }
            continue;
        }
        reaction_waiting |= is_reaction;
        g_ev_retry.push_back(std::move(e));
    }
}

void on_connected(std::uint32_t) {
    g_ring = std::make_unique<std::array<snap::Frame, kRing>>();
    // FR-NET-2c: the guest brings its own build (equipment from its own save); progress / unlocks are the
    // host's shared progress. Only equipment fields travel.
    json::Value root = json::Value::object();
    root.set("guestKey", guest_key());
    if (auto prof = core::profile::read(world::p1_controller(), nullptr)) {
        core::profile::Profile b;
        b.spirits = prof->spirits;
        b.summon_set = prof->summon_set;
        if (prof->passives && prof->passives->equipped) b.passives = core::profile::Passives{prof->passives->equipped, std::nullopt, std::nullopt};
        auto build = core::profile::to_json(b);
        g_equip_sent = json::dump(build, 0);
        root.set("build", std::move(build));
    }
    std::vector<std::uint8_t> big(proto::kMaxJson + 64);
    if (const auto n = proto::encode(proto::Msg::Build, proto::Json{json::dump(root, 0)}, big)) coop::send_reliable(0, std::span(big.data(), n));
}

void on_reliable(std::uint32_t, std::span<const std::uint8_t> msg) {
    switch (proto::type_of(msg)) {
    case proto::Msg::SpiritFrame: {
        proto::SpiritFrame f;if(!proto::decode(msg,f))return coop::leave("invalid autonomous frame");
        if(!g_refresh_pending && f.context.epoch==g_epoch&&f.context.map==g_host_map&&f.context.view==g_host_view)autonomous::receive(f);return;
    }
    case proto::Msg::LilyCauses: {
        proto::LilyCauses batch;if(!proto::decode(msg,batch))return coop::leave("invalid causal batch");
        if(!g_refresh_pending && batch.epoch==g_epoch&&batch.map==g_host_map&&batch.view==g_host_view){causal_api();if(!causality::receive(batch))coop::leave("causal facts invalid or conflicting");}return;
    }
    case proto::Msg::LilyBuild: {
        proto::Json m;
        if (!proto::decode(msg, m)) return void(coop::leave("invalid native Lily build"));
        std::string why;
        json::Value root;
        if (!json::parse(m.text, root, &why) || !root.get("seat") || !root.get("map") || !root.get("profile")) return void(coop::leave("native Lily build layout"));
        const auto seat = static_cast<std::uint16_t>(root.get("seat")->number_or(0));
        if (seat != g_seat && root.get("map")->number_or(0) == g_host_map && root.get("view") && root.get("view")->number_or(0) == g_host_view)
            native_lily::build(seat, json::dump(*root.get("profile"), 0));
        return;
    }
    case proto::Msg::LilyFrame: {
        proto::LilyFrame value;
        if (!proto::decode(msg, value)) return void(coop::leave("invalid native Lily frame"));
        if (!g_refresh_pending && value.seat != g_seat && value.map == g_host_map && value.epoch==g_epoch && value.view==g_host_view) native_lily::receive(value);
        return;
    }
    case proto::Msg::Welcome: {
        proto::Welcome w;
        if (!proto::decode(msg, w)) return;
        g_seat = w.seat, g_epoch = w.epoch, g_host_map = w.map;
        if (!party::ui::enable_client()) return void(coop::leave("Client menu/input boundary unavailable"));
        ability::reset_fault();
        g_seat_placed = false;
        if (!guard::arm()) return void(coop::leave("guard unavailable (fail closed)"));
        if (!reaction::client_enable(true)) return void(coop::leave("native knockback boundary unavailable"));
        g_reaction_last = g_reaction_time = 0;
        g_reaction_anchor = false;
        g_reaction_failure.clear();
        if(!core::permission::p1_presentation(true))return void(coop::leave("Client world-interaction boundary unavailable"));
        if (!fx::client_enable()) return void(coop::leave("native feedback capture/suppression unavailable"));
        log::info("coop: seated as P{} on the host (epoch {})", g_seat, g_epoch);
        break;
    }
    case proto::Msg::NameDef: {
        proto::NameDef d;
        if (proto::decode(msg, d)) g_names.define(d.id, d.text);
        break;
    }
    case proto::Msg::Map: {
        proto::MapMsg m;
        if (!proto::decode(msg, m) || !g_seat) return;
        if (m.map != g_host_map || m.view != g_host_view) {
            reset_remote_world();
            g_refresh_pending = false;
        }
        g_host_map = m.map;
        g_host_view = m.view;causal_api();
        g_follow_tag = m.tag;
        g_follow_pending = true;
        g_follow_next_us = 0;
        break;
    }
    case proto::Msg::Act:
    case proto::Msg::Reaction:
    case proto::Msg::ChildStop:
    case proto::Msg::Fx:
    case proto::Msg::FxStop:
    case proto::Msg::Snd:
    case proto::Msg::ActStop: {
        // after leave (stop() ran in this same session tick, enemies released, OpenLevel queued) the inbox
        // can still hold events: never replay outside a seated session (AV 0x130 in LaunchAbilityFromDescription)
        if (!g_seat) return;
        // A stop never launches gameplay. Cancel a pending spawn even during loading, so a queued
        // owner retry cannot resurrect a lifecycle that ended in the same network/game frame.
        // the local world is loading / tearing down (an act replayed while the client went back to the title
        // crashed inside LaunchAbilityFromDescription: AV 0x130)
        if (proto::type_of(msg) != proto::Msg::Reaction && proto::type_of(msg) != proto::Msg::ChildStop && proto::type_of(msg) != proto::Msg::FxStop && !feedback_message(msg) && !world_ready()) return void(++g_act_absent);
        // Accepted reactions are causal and ordered. A later reaction cannot pass one that
        // still waits for its map/asset context, even though both messages arrived reliably.
        const bool waiting_reaction = proto::type_of(msg) == proto::Msg::Reaction &&
            std::any_of(g_ev_retry.begin(), g_ev_retry.end(), [](const RetryEv& e) { return proto::type_of(e.msg) == proto::Msg::Reaction; });
        if (waiting_reaction || handle_event(msg) == EvResult::Retry) {
            if (g_ev_retry.size() < 64) g_ev_retry.push_back({clock::now_us() + kRetryUs, std::vector<std::uint8_t>(msg.begin(), msg.end())});
            else {
                ++g_ev_absent;
                if (proto::type_of(msg) == proto::Msg::Reaction)
                    ++g_reaction_failed, g_reaction_failure = "authoritative reaction queue exhausted";
                if (proto::type_of(msg) == proto::Msg::ChildStop)
                    ++g_child_failed, g_event_failure = "authoritative child stop queue exhausted";
                if (proto::type_of(msg) == proto::Msg::Act)
                    g_event_failure = "authoritative ability launch queue exhausted";
                if (feedback_message(msg)) g_event_failure = "authoritative feedback queue exhausted";
                if (proto::type_of(msg) == proto::Msg::FxStop) g_event_failure = "authoritative FX stop queue exhausted";
            }
        }
        break;
    }
    case proto::Msg::EquipResult: {
        proto::Json j;
        if (proto::decode(msg, j)) g_equip_result = j.text, log::info("coop: host applied our equipment: {}", j.text);
        break;
    }
    case proto::Msg::Control: {
        proto::Control c;
        if (!proto::decode(msg, c)) break;
        if (c.code == proto::kControlBanner) overlay::set_banner(c.text); // mode status (hunter compass)
        else log::warn("coop: host says: {}", c.text);
        break;
    }
    default: break;
    }
}

void on_unreliable(std::uint32_t, std::uint8_t stream, std::span<const std::uint8_t> msg) {
    if (stream != proto::kSnapStream || !g_ring) return;
    std::uint32_t seq = 0, base = 0;
    if (!snap::peek(msg, seq, base) || seq <= g_latest) return;
    const snap::Frame* b = nullptr;
    if (base) {
        b = &(*g_ring)[base % kRing];
        if (b->seq != base) return void(++g_bad_snaps); // base gone: wait for a delta against a newer ack
    }
    auto& slot = (*g_ring)[seq % kRing];
    if (b == &slot) return void(++g_bad_snaps);
    if (!snap::decode(msg, b, slot)) return void(++g_bad_snaps);
    g_latest = seq;
    note_host_time(slot.t_ms, clock::now_us());
    if (lifecycle::state() == lifecycle::State::Settled) apply(slot);
}

// Map follow (v1 net-facts「跟图」): only when the local loader is idle and the map is ready; the vanilla
// SwitchGameMap with the host's row + entry tag; MapReady once the local map equals the host map.
void follow(std::int64_t now_us) {
    if (!g_follow_pending || now_us < g_follow_next_us) return;
    const auto* host_map = g_names.text(g_host_map);
    if (!host_map) return;
    bool loading = true, ready = false;
    if (!world::loader_flags(loading, ready) || loading || !ready) return;
    if (*host_map == ue::utf8(world::current_map_id())) {
        coop::send_reliable(0, std::span(g_buf.data(), proto::encode(proto::MapReady{g_host_map,g_host_view}, g_buf)));
        g_follow_pending = false;
        g_seat_placed = false;
        return;
    }
    g_follow_next_us = now_us + 2'000'000; // ≥ 2 s between attempts
    auto* wl = world::world_loader();
    static reflect::FuncRef sw;
    if (!sw.ok()) {
        reflect::Resolver r("WorldLoaderSubsystem", reflect::find_class(L"/Script/Zenith.WorldLoaderSubsystem"));
        sw = r.func(L"SwitchGameMap", 4);
    }
    auto* table = reflect::find_object(L"/Game/_Zenith/Gameplay/Data/DT_GameMapData.DT_GameMapData");
    if (!wl || !sw.ok() || !table) return void(log::warn("coop: map follow unavailable (loader / SwitchGameMap / DT_GameMapData)"));
    struct Handle {
        ue::UObject* table;
        ue::FName row;
    } h{table, ue::FName(log::wide(*host_map).c_str(), RC::Unreal::FNAME_Add)};
    const ue::FName tag(log::wide(g_follow_tag.empty() ? "0" : g_follow_tag).c_str(), RC::Unreal::FNAME_Add);
    reflect::Frame f(sw);
    f.arg(0, h).arg(1, static_cast<ue::UObject*>(nullptr)).arg(2, static_cast<ue::UObject*>(nullptr)).arg_name(3, tag);
    f.invoke(wl);
    log::info("coop: following the host to {} (tag {})", *host_map, g_follow_tag);
}

// 1 Hz recheck (G1): every taken enemy's two activation flags (ZenithAIController.bActive,
// ZenithCharacter.bActivated — EL.json offsets, reflection-checked) must stay false on the client.
// A leak is counted and the actor re-frozen (tick off again; the AI activation itself is gated).
void recheck(std::int64_t now_us) {
    static clock::Cadence every(1000);
    if (!every.due(now_us)) return;
    static const auto ai_off = binary_checks::offset("ZenithAIController.bActive");
    static const auto pawn_off = binary_checks::offset("ZenithCharacter.bActivated");
    ++g_rechecks;
    if (!ai_off || !pawn_off) return;
    for (auto& [n, t] : g_taken) {
        auto* e = t.enemy.get();
        auto* c = t.controller.get();
        const bool ai = c && *(reinterpret_cast<const std::uint8_t*>(c) + *ai_off) & 1;
        const bool pawn = e && *(reinterpret_cast<const std::uint8_t*>(e) + *pawn_off) & 1;
        if (ai) {
            ++g_leaks;
            t.ready = presentation_policy::apply(e);
            if (!t.ready) entities::hide_actor(e);
        }
        g_pawn_activated += pawn; // informative: vanilla sets bActivated on spawn (presentation only)
    }
}

// Equipment route (F2.10): the guest edits its build with the vanilla menus on its local P1; at 1 Hz the
// equipment fields are compared and a change goes to the host as EquipRequest (host applies, answers).
std::string equipment_json() {
    auto prof = core::profile::read(world::p1_controller(), nullptr);
    if (!prof) return {};
    core::profile::Profile b;
    b.spirits = prof->spirits;
    b.summon_set = prof->summon_set;
    if (prof->passives && prof->passives->equipped) b.passives = core::profile::Passives{prof->passives->equipped, std::nullopt, std::nullopt};
    return json::dump(core::profile::to_json(b), 0);
}

void equip_poll(std::int64_t now_us) {
    static clock::Cadence every(1000);
    if (!every.due(now_us)) return;
    auto cur = equipment_json();
    if (cur.empty()) return;
    if (cur == g_equip_sent) return;
    g_equip_sent = cur;
    std::vector<std::uint8_t> big(proto::kMaxJson + 64);
    if (const auto n = proto::encode(proto::Msg::EquipRequest, proto::Json{cur}, big)) coop::send_reliable(0, std::span(big.data(), n)), ++g_equip_requests;
}

// Enemy motion presentation (§16 interpolation): render host time = local clock mapped onto the host clock
// (offset tracks the earliest arrival: max of host_t − local, slowly decaying) minus a 100 ms buffer; each
// taken enemy is placed between the two samples that bracket it. Gaps > 500 ms jump straight to the new
// sample (no slow slide after silence); past the newest sample it holds (no extrapolation).
std::string g_trace_name;
std::vector<float> g_trace;
double g_offset_ms = 0;
bool g_offset_known = false;
std::uint64_t g_motion_writes = 0;

void note_host_time(std::uint32_t host_ms, std::int64_t now_us) {
    const double est = static_cast<double>(host_ms) - static_cast<double>(now_us) / 1000.0;
    if (!g_offset_known || est > g_offset_ms) g_offset_ms = est, g_offset_known = true;
    else g_offset_ms -= 0.02; // drift allowance
}

void present_motion(std::int64_t now_us) {
    const auto* s = core::schema::script();
    if (!s || !g_offset_known) return;
    const double render = causality::playback_ms()>0?causality::playback_ms():static_cast<double>(now_us) / 1000.0 + g_offset_ms - 50.0;
    // a position write costs ~80 µs (actor + Spine children transform update, net-facts「敌人位置插值」):
    // enemies farther than 3000 UU from the local lily (off-screen) are written at ~10 Hz only
    static std::uint32_t frame = 0;
    ++frame;
    const auto me = core::location(world::p1_pawn());
    int idx = 0;
    for (auto& [n, t] : g_taken) {
        ++idx;
        if (!t.ready || !t.n || t.hidden) continue;
        auto* e = t.enemy.get();
        if (!e) continue;
        if(causality::target_owned(e))continue; // exact target context is the sole motion writer while it is in use
        if (me) {
            const auto& last = t.s[(t.head + t.n - 1) % 16];
            if (std::fabs(last.y - me->y) + std::fabs(last.z - me->z) > 3000.0f && (frame + idx) % 6) continue;
        }
        const Sample* a = &t.s[t.head];
        const Sample* b = a;
        for (int i = 1; i < t.n; ++i) {
            const auto& c = t.s[(t.head + i) % 16];
            if (c.t <= render) a = &c;
            b = &c;
            if (c.t > render) break;
        }
        core::Vec3 p{a->x, a->y, a->z};
        if (b != a && b->t > a->t && b->t - a->t <= 500 && render > a->t) {
            const float k = static_cast<float>(std::min(1.0, (render - a->t) / static_cast<double>(b->t - a->t)));
            p = {a->x + (b->x - a->x) * k, a->y + (b->y - a->y) * k, a->z + (b->z - a->z) * k};
        } else if (b != a && b->t - a->t > 500 && render >= a->t) {
            p = {b->x, b->y, b->z};
        }
        if (n == g_trace_name && g_trace.size() < 240) g_trace.push_back(p.y); // Dev: per-frame presented Y
        if (auto at = core::location(e); at && (std::fabs(at->x - p.x) + std::fabs(at->y - p.y) + std::fabs(at->z - p.z)) > 0.2f) {
            reflect::Frame tp(s->actor_teleport);
            tp.arg(0, p).arg_bool(1, false).arg_bool(3, false); // TeleportPhysics resets every component's physics state (Spine procedural mesh) → ~80 µs; plain move
            const auto w0 = clock::now_us();
            tp.invoke(e);
            h_write.add(static_cast<double>(clock::now_us() - w0));
            ++g_motion_writes;
        }
    }
}

void refresh_view(std::string reason) {
    const auto size = proto::encode(proto::RefreshView{g_host_map, g_host_view}, g_buf);
    if (!size) return coop::leave(reason);
    log::warn("coop: playback refresh P{} map={} view={} reason={} queuedEvents={} {}", g_seat, g_host_map, g_host_view,
              reason, g_ev_retry.size(), native_lily::describe());
    g_last_refresh = std::move(reason);
    g_refresh_pending = true;
    ++g_refreshes;
    coop::send_reliable(0, std::span(g_buf.data(), size));
}
void tick(std::int64_t now_us) {
    if (g_refresh_pending) {
        if (lifecycle::state() == lifecycle::State::Settled) send_input();
        return;
    }
    causal_api();
    if (causality::playback_gap()) return refresh_view(causality::fault());
    if (native_lily::playback_gap()) return refresh_view(native_lily::fault());
    if(const auto why=causality::fault();!why.empty())return coop::leave(why);
    if (g_seat) if (const auto fault = fx::fault(); !fault.empty()) g_event_failure = fault;
    if (g_seat) {
        const auto fault = ability::fault();
        if (!fault.empty()) g_event_failure = fault;
    }
    if (g_seat && !g_event_failure.empty()) {
        const auto why = g_event_failure;
        coop::leave(why);
        return;
    }
    if (g_seat && !g_reaction_failure.empty()) {
        const auto why = g_reaction_failure;
        coop::leave(why);
        return;
    }
    if (!g_seat || lifecycle::state() != lifecycle::State::Settled) return;
    auto t0 = clock::now_us();
    present_motion(now_us);
    auto t1 = clock::now_us();
    h_motion.add(static_cast<double>(t1 - t0));
    equip_poll(now_us);
    auto t2 = clock::now_us();
    send_input();
    follow(now_us);
    if(!autonomous::client_tick(g_seat,g_host_map,[](std::uint16_t id){return g_names.text(id);}))return coop::leave(autonomous::fault());
    native_lily::client_tick(g_host_map, [](std::uint16_t id) { return g_names.text(id); });
    if (const auto why = native_lily::fault(); !why.empty()) return coop::leave(why);
    auto t3 = clock::now_us();
    h_input.add(static_cast<double>(t3 - t2));
    recheck(now_us);
    retry_events(now_us);
    static clock::Cadence prune_families(100);
    if (prune_families.due(now_us))
        std::erase_if(g_ab_live, [](const auto& entry) { return !ability::has_active_family(entry.second.activation); });
    if (t2 - t1 + clock::now_us() - t3 > 50) h_slow.add(static_cast<double>(t2 - t1 + clock::now_us() - t3));
}

void stop() {
    party::ui::disable_client();
    const bool was_seated = g_seat != 0;
    reset_remote_world();

    // Teardown can invoke vanilla callbacks; only disarm after every remote object/result is retired.
    reaction::client_enable(false);
    reaction::reset();
    guard::disarm();
    fx::client_disable();
    ability::reset_fault();
    core::permission::p1_presentation(false);
    overlay::set_banner({});
    g_seat = 0, g_seat_placed = false, g_epoch = 0, g_latest = 0, g_host_map = 0;
    g_host_view = 0;
    g_refresh_pending = false;
    g_reaction_last = g_reaction_time = 0;
    g_reaction_seats.fill(0);
    g_reaction_anchor = false;
    g_reaction_failure.clear();
    g_follow_pending = false;
    g_follow_next_us = 0;
    g_follow_tag.clear();
    g_ring.reset();
    g_names.clear();
    g_equip_sent.clear();
    g_offset_known = false;
    g_offset_ms = 0;
    g_input = {};
    // The local world was a presentation of the host's (enemies frozen with AI gated, objects hidden):
    // fail closed by returning to the title instead of continuing single-player on it (P2 decision).
    if (was_seated && lifecycle::state() == lifecycle::State::Settled) { // a rejected / never-seated join changed nothing
        static reflect::FuncRef open = reflect::Resolver("GameplayStatics", reflect::find_class(L"/Script/Engine.GameplayStatics")).func(L"OpenLevel", 4);
        auto* cdo = reflect::find_object(L"/Script/Engine.Default__GameplayStatics");
        if (open.ok() && cdo) {
            reflect::Frame f(open);
            f.arg(0, world::game_instance()).arg_name(1, ue::FName(L"TitleMap", RC::Unreal::FNAME_Add)).arg_bool(2, true).arg_string(3, L"");
            f.invoke(cdo);
            log::info("coop: left the session — back to the title (the local world was a presentation)");
        }
    }
}

void on_closed(std::uint32_t, const std::string&, net::Reject) {}

} // namespace

// enemy_dyn client half: ed row key → local enemy (adopted unbound same-class enemy ≤ 300 UU, or a spawned
// presentation copy). Key in g_taken = "dyn:<id>".
struct Dyn {
    ObjHandle actor;
    bool spawned = false, seen = false;
};
std::map<std::uint16_t, Dyn> g_dyn;
std::uint64_t g_dyn_adopted = 0, g_dyn_spawned = 0, g_dyn_removed = 0;

namespace {
void reset_dynamic() {
    for (auto& [id, d] : g_dyn) {
        if (auto* actor = d.actor.get()) {
            release_presentation(actor);
            if (d.spawned) entities::destroy_copy(actor);
            else entities::hide_actor(actor);
        }
        g_taken.erase(std::format("dyn:{}", id));
        ++g_dyn_removed;
    }
    g_dyn.clear();
}
} // namespace

void present_dynamic(const ClientCtx& c) {
    const auto* s = core::schema::script();
    if (!s || !g_resolved) return;
    for (auto& [k, d] : g_dyn) d.seen = false;
    for (const auto& row : c.frame.ed.view()) {
        auto& d = g_dyn[row.key];
        d.seen = true;
        auto* e = d.actor.get();
        if (!e) {
            d.spawned = false; // a replacement may adopt a local actor even if its predecessor was ours
            const auto* path = c.text(row.type);
            if (!path) continue;
            auto* cls = entities::class_by_path(*path);
            if (!cls) continue;
            for (const auto& en : registry::bucket(registry::Kind::Enemy)) { // adopt
                auto* o = en.handle.get();
                if (!o || o->GetClassPrivate() != cls || g_spawn_point.object(o)) continue;
                bool bound = false;
                for (const auto& [k2, d2] : g_dyn) bound |= d2.actor == en.handle;
                const auto at = core::location(o);
                if (!bound && at && std::fabs(at->y - row.y) + std::fabs(at->z - row.z) <= 300.0f) {
                    e = o;
                    ++g_dyn_adopted;
                    break;
                }
            }
            if (!e && (e = entities::spawn_copy(cls, row.x, row.y, row.z))) d.spawned = true, ++g_dyn_spawned;
            if (!e) continue;
            d.actor = ObjHandle::of(e);
        }
        present_enemy(std::format("dyn:{}", row.key), e, row, c.frame, *s);
    }
    for (auto it = g_dyn.begin(); it != g_dyn.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        if (auto* e = it->second.actor.get()) {
            release_presentation(e);
            if (it->second.spawned) entities::destroy_copy(e);
            else entities::hide_actor(e);
        }
        g_taken.erase(std::format("dyn:{}", it->first));
        ++g_dyn_removed;
        it = g_dyn.erase(it);
    }
}

void present_enemies(const ClientCtx& c) {
    if (const auto* s = core::schema::script(); s && g_resolved) present_enemies_impl(c.frame, *s);
}

ue::UObject* actor_of(std::uint8_t kind, std::uint16_t id, bool allow_dying, bool feedback) {
    ue::UObject* a = nullptr;
    switch (static_cast<proto::RefKind>(kind)) {
    case proto::RefKind::Enemy: {
        // Abilities require a live owner. Host FX/sounds may still belong to the visible death phase;
        // HP reaching zero must not discard those outputs before the Host removes/hides the actor.
        const auto* name = g_names.text(id);
        auto t = name ? g_taken.find(*name) : g_taken.end();
        if (t != g_taken.end() && t->second.ready && !t->second.hidden && (allow_dying || !t->second.killed)) a = t->second.enemy.get();
        break;
    }
    case proto::RefKind::Dyn: {
        auto t = g_taken.find(std::format("dyn:{}", id));
        if (t != g_taken.end() && t->second.ready && !t->second.hidden && (allow_dying || !t->second.killed)) a = t->second.enemy.get();
        break;
    }
    case proto::RefKind::Lily:
        if (feedback && g_seat && id == g_seat) a = world::p1_pawn();
        else if (auto it = g_copies.find(id); it != g_copies.end() && it->second.ready) a = core::pawn(it->second.slot);
        break;
    case proto::RefKind::Spirit:
    case proto::RefKind::Companion:
        break;
    }
    return a && !a->HasAnyFlags(RC::Unreal::RF_BeginDestroyed) ? a : nullptr;
}
namespace {
ue::UObject* causal_target(std::uint8_t kind,std::uint16_t id,std::uint32_t generation) {
    const auto* name=kind==1?g_names.text(id):nullptr;
    const auto key=kind==1?(name?*name:std::string{}):std::format("dyn:{}",id);
    auto it=g_taken.find(key);if(it==g_taken.end()||it->second.instance!=generation)return nullptr;
    return it->second.enemy.get();
}
void causal_motion(double) { present_motion(clock::now_us()); }
}

std::string describe_events() {
    return std::format("events(client): spirits={} spawned={} failed={} retryQueue={} retried={} absent={} fxBad={} soundBad={} soundFar={} stopMissed={} live abilities={} fx={} fxKept={} fxReplaced={}\n",
                       std::size_t{0}, std::uint64_t{0}, std::uint64_t{0}, g_ev_retry.size(), g_ev_retried, g_ev_absent, g_fx_bad, g_snd_bad, g_snd_far, g_stop_missed, g_ab_live.size(),
                       g_fx_live.size(), g_fx_kept, g_fx_replaced) +
           std::format("ability children: stopped={} alreadyEnded={} failed={} fault='{}'\n", g_child_stopped, g_child_ended, g_child_failed, g_event_failure) +
           std::format("reaction(client): received={} stale={} failed={} last={} reason='{}'\n", g_reaction_received, g_reaction_stale,
                       g_reaction_failed, g_reaction_last, g_reaction_failure) + reaction::describe();
}

void present_lily_rows(const ClientCtx& c) {
    if (const auto* s = core::schema::script()) present_lilies(c.frame, *s);
}

std::string describe_dynamic() {
    return std::format("enemy_dyn(client): bound={} adopted={} spawned={} removed={}\n", g_dyn.size(), g_dyn_adopted, g_dyn_spawned, g_dyn_removed);
}

std::string trace(const std::string& name) {
    std::string out;
    for (float y : g_trace) out += std::format("{:.1f} ", y);
    g_trace_name = name;
    g_trace.clear();
    return out.empty() ? "(tracing " + name + ")" : out;
}

std::string adapters_text() {
    std::string out;
    for (auto* a : adapters()) out += a->describe();
    return out;
}

std::string describe() {
    return party::ui::client_status() + std::format("client: seat=P{} epoch={} latest={} names={} taken={} applied={} badSnaps={} unknownNames={} copies={} reconciles={} kills={} actBad={} actAbsent={} rechecks={} aiLeaks={} world={} equipRequests={} motionWrites={} presses(attack)={}\n{}{}", g_seat,
                       g_epoch, g_latest, g_names.size(), g_taken.size(), g_applied, g_bad_snaps, g_unknown_names, g_copies.size(), g_reconciles, g_kills, g_act_bad, g_act_absent, g_rechecks, g_leaks, std::uint64_t{0}, g_equip_requests, g_motion_writes, g_input.presses[3], presentation_policy::describe() + presentation::describe() + visual_state::describe() + native_lily::describe() + ability::describe() + fx::describe() + describe_events() + adapters_text(),
                       guard::describe()) + std::format("view={} refreshes={} waiting={} last='{}'\n",g_host_view,g_refreshes,g_refresh_pending,g_last_refresh);
}

void install() {
    coop::set_handlers(Role::Client, {on_connected, on_closed, on_reliable, on_unreliable, tick, stop});
}

} // namespace elmp::coop::client
