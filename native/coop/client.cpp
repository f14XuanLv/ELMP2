// Coop client (first slice): guest key → Build; Welcome arms the guard; every frame the local P1 input
// snapshot goes upstream (axes, held bits, cumulative press counters, snapshot ack); snapshots are decoded
// against the acknowledged base; enemies named by their spawn point are taken over (guard mark on pawn +
// controller, actor tick off, CanBePaused untouched by vanilla since activation is gated) and placed /
// HP-written from the host rows. The client process never settles (guard), never saves (guard).

#include "coop/client.h"

#include "coop/ability.h"
#include "coop/fx.h"
#include "overlay/render.h"
#include "coop/entities/clearable.h"
#include "coop/entities/spawn.h"
#include "coop/entity.h"
#include "coop/session.h"
#include "coop/sync.h"

#include "guard/guard.h"

#include "core/player.h"
#include "core/permission.h"
#include "core/profile.h"
#include "core/schema.h"
#include "core/spawn.h"

#include "engine/binary_checks.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "input/pad_state.h"

#include "net/protocol.h"
#include "net/snapshot.h"

#include "base/clock.h"
#include "base/json.h"
#include "base/log.h"
#include "base/metrics.h"

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
ue::UObject* actor_of(std::uint8_t kind, std::uint16_t id);

namespace {

namespace snap = net::snap;
namespace proto = net::proto;

constexpr std::size_t kRing = 32;
constexpr float kReconcileUU = 150.0f; // local P1 vs host seat drift before a snap (decision P2: prediction + snap)
std::uint64_t g_reconciles = 0;

void note_host_time(std::uint32_t host_ms, std::int64_t now_us);

struct Sample {
    std::uint32_t t = 0; // host ms
    float x = 0, y = 0, z = 0;
};
struct Taken {
    std::array<Sample, 16> s{}; // ≥ 100 ms buffer at 60 Hz host capture (4 was too few: held the oldest sample, stepped on rotation)
    int head = 0, n = 0;
    ObjHandle enemy, controller, spine;
    std::uint16_t anim = 0;
    bool killed = false, hidden = false;
    std::int8_t facing = 0;
    std::uint32_t frame = 0; // last snapshot that carried the row
    std::int32_t hp = -1;    // last presented HP (hit reaction on a drop)
};

// Hit reaction (P6): the vanilla OnReceiveDamage handler of BP_Character_SpineBase (additive damage
// animation unless in knockback) run on the presented actor when its host HP drops. The client's HPComponent
// never settles (guard), so the delegate never fires by itself; HP was already written from the row.
std::uint64_t g_hit_reacts = 0;
void hit_react(ue::UObject* actor, std::int32_t damage) {
    static std::unordered_map<ue::UObject*, reflect::FuncRef> per_class; // class → handler (fn null = none)
    auto* cls = actor ? actor->GetClassPrivate() : nullptr;
    if (!cls) return;
    auto [it, fresh] = per_class.try_emplace(cls);
    if (fresh) it->second = reflect::Resolver("SpineBase", cls).func(L"BndEvt__HPComponent_K2Node_ComponentBoundEvent_0_HPDamageEvent__DelegateSignature", 2);
    if (!it->second.ok()) return;
    reflect::Frame f(it->second);
    f.arg(0, static_cast<ue::UObject*>(nullptr)).arg(1, damage);
    if (f.invoke(actor)) ++g_hit_reacts;
}

std::uint8_t g_seat = 0;
bool g_seat_placed = false;
std::uint16_t g_epoch = 0, g_host_map = 0;
std::uint32_t g_latest = 0;
std::string g_follow_tag;
std::string g_equip_sent, g_equip_result;
std::uint64_t g_equip_requests = 0;
bool g_follow_pending = false;
std::int64_t g_follow_next_us = 0;
std::unique_ptr<std::array<snap::Frame, kRing>> g_ring;
proto::NameTable g_names;
proto::Input g_input;
std::array<std::uint8_t, 2048> g_buf;
std::unordered_map<std::string, Taken> g_taken; // spawn point name → local enemy
std::uint64_t g_index_version = ~0ull;
std::unordered_map<std::string, ObjHandle> g_index; // spawn point name → enemy (rebuilt on registry change)
reflect::PropRef g_spawn_point;
reflect::FuncRef g_set_tick, g_set_collision, g_set_rotation;
bool g_resolved = false;
std::uint64_t g_unknown_names = 0, g_bad_snaps = 0, g_applied = 0, g_anim_sets = 0, g_anim_fail = 0, g_kills = 0, g_act_bad = 0, g_act_absent = 0, g_rechecks = 0, g_leaks = 0, g_pawn_activated = 0;
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
    const auto* s = core::schema::script();
    auto* p1 = world::p1_pawn();
    const auto off = binary_checks::offset("ZenithInputComponent.InputSnapshot");
    auto* ic = s && p1 ? s->zc_input.object(p1) : nullptr;
    if (ic && off) {
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

// Freeze every component except presentation (v1 net-facts「会话与呈现」whitelist); the Spine state
// machines (enemy SpineAnimationComponent / Lily animation logic) would otherwise overwrite track 0.
void freeze(ue::UObject* enemy) {
    static reflect::FuncRef set_comp_tick;
    static ObjHandle comp_cls;
    if (!set_comp_tick.ok()) {
        reflect::Resolver r("ActorComponent", reflect::find_class(L"/Script/Engine.ActorComponent"));
        set_comp_tick = r.func(L"SetComponentTickEnabled", 1);
        comp_cls = ObjHandle::of(reflect::find_object(L"/Script/Engine.ActorComponent"));
    }
    const auto* s = core::schema::script();
    if (s && set_comp_tick.ok()) {
        reflect::Frame f(s->actor_components_by_class);
        f.arg(0, comp_cls.get());
        if (f.invoke(enemy))
            if (const auto* arr = static_cast<const reflect::RawArray*>(f.raw(s->actor_components_by_class.ret)); arr && arr->num >= 0 && arr->num <= 128) {
                const auto* items = reinterpret_cast<ue::UObject* const*>(arr->data);
                for (int i = 0; i < arr->num; ++i) {
                    if (!ue::alive(items[i])) continue;
                    const auto cls = ue::class_name_of(items[i]);
                    if (cls == L"SpineSkeletonAnimationExComponent" || cls == L"SpineSkeletonRendererComponent" || cls == L"AbilityComponent" ||
                        cls == L"SpineBoneFollowerComponent" || cls == L"SpineBoneComponent" || cls == L"ParticleSystemComponent" || cls == L"WidgetComponent") {
                        // dormant enemies ship with the Spine animation / renderer ticks off → turn the
                        // presentation set on explicitly (v1 net-facts: else the mesh stays at its first frame)
                        reflect::Frame on(set_comp_tick);
                        on.arg_bool(0, true);
                        on.invoke(items[i]);
                        continue;
                    }
                    reflect::Frame t(set_comp_tick);
                    t.arg_bool(0, false);
                    t.invoke(items[i]);
                }
            }
    }
    if (g_set_tick.ok()) {
        reflect::Frame f(g_set_tick);
        f.arg_bool(0, false);
        f.invoke(enemy);
    }
}

void take(const std::string& name, ue::UObject* enemy) {
    auto* controller = core::schema::script() ? core::schema::script()->pawn_controller.object(enemy) : nullptr;
    guard::mark(enemy);
    guard::mark(controller);
    freeze(enemy);
    Taken t;
    t.enemy = ObjHandle::of(enemy);
    t.controller = ObjHandle::of(controller);
    g_taken[name] = t;
}

void present_lilies(const snap::Frame& f, const core::schema::Script& s);

// One enemy row (adapter enemy; also used by enemy_dyn for adopted / spawned copies): takeover on first
// sight, facing, buffered position, HP, Spine pose.
void present_enemy(const std::string& key, ue::UObject* enemy, const snap::EnemyRow& row, const snap::Frame& f, const core::schema::Script& s_) {
    const auto* s = &s_;
        if (auto t = g_taken.find(key); t == g_taken.end() || t->second.enemy.get() != enemy) take(key, enemy);
    auto& t = g_taken[key];
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
        if (t.hp >= 0 && row.hp < t.hp && !t.killed) hit_react(enemy, t.hp - row.hp);
        t.hp = row.hp;
        if ((row.flags & 4) && !t.killed) t.killed = true, ++g_kills;
    }
    t.frame = g_latest;
    if (row.anim && row.anim != t.anim) { // pose: switch track 0 only on an animation change (§16)
        const auto* anim = g_names.text(row.anim);
        if (!t.spine.get()) {
            static ObjHandle cls = ObjHandle::of(reflect::find_object(L"/Script/SpineExtensionPlugin.SpineSkeletonAnimationExComponent"));
            reflect::Frame f(s->actor_component_by_class);
            f.arg(0, cls.get());
            if (f.invoke(enemy)) t.spine = ObjHandle::of(f.ret<ue::UObject*>());
        }
        if (auto* spine = t.spine.get(); spine && anim) {
            reflect::Frame f(s->sp_set_animation);
            f.arg(0, 0).arg_string(1, log::wide(*anim)).arg_bool(2, row.loop != 0);
            if (f.invoke(spine)) {
                ++g_anim_sets;
                // align the new track to the host's playback time (no restart-from-zero phase lag)
                static reflect::FuncRef set_time =
                    reflect::Resolver("TrackEntry", reflect::find_class(L"/Script/SpinePlugin.TrackEntry")).func(L"SetTrackTime", 1);
                if (auto* entry = f.ret<ue::UObject*>(); set_time.ok() && ue::alive(entry) && row.t > 0) {
                    reflect::Frame st(set_time);
                    st.arg(0, row.t);
                    st.invoke(entry);
                }
            } else {
                ++g_anim_fail;
            }
            t.anim = row.anim;
        }
    }
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
    std::uint16_t anim = 0;
    std::int8_t facing = 0;
    bool seen = false;
    std::int32_t hp = -1;
};
std::map<std::uint16_t, Copy> g_copies;

void drop_copy(std::uint16_t key) {
    auto it = g_copies.find(key);
    if (it == g_copies.end()) return;
    if (auto* p = core::pawn(it->second.slot)) guard::unmark(p), guard::mark_not_player(p, false);
    core::spawn::despawn(it->second.slot, false);
    core::leases().release({"ELMP", slots::OwnerKind::Elmp}, it->second.slot);
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
    const auto* bp = core::schema::blueprint();
    for (const auto& row : f.l.view()) {
        if (row.key == g_seat) { // own seat: the local P1 predicts; snap on first placement or when it drifts from the host seat
            auto* p1 = world::p1_pawn();
            if (auto at = p1 ? core::location(p1) : std::nullopt;
                at && (!g_seat_placed || std::hypot(at->y - row.y, at->z - row.z) > kReconcileUU)) {
                reflect::Frame tp(s.actor_teleport);
                tp.arg(0, core::Vec3{row.x, row.y, row.z}).arg_bool(1, false).arg_bool(3, true);
                if (tp.invoke(p1)) g_seat_placed = true, ++g_reconciles;
            }
            if (auto* hp = p1 ? s.zc_hp.object(p1) : nullptr) { // own vitals are the host seat's (HUD shows them)
                *s.hp_max.at<std::int32_t>(hp) = row.mhp;
                *s.hp_curr.at<std::int32_t>(hp) = std::max<std::int32_t>(row.hp, row.alive ? 1 : 0);
            }
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
            if (auto e = core::spawn::spawn(slot, o); !e.empty()) {
                log::warn("coop: copy of P{} failed: {}", row.key, e);
                continue;
            }
            auto* p = core::pawn(slot);
            if (g_set_collision.ok()) {
                reflect::Frame fc(g_set_collision);
                fc.arg_bool(0, false);
                fc.invoke(p);
            }
            resolve();
            freeze(p);
            guard::mark(p);
            guard::mark_not_player(p, true);
            c.anim = 0, c.facing = 0;
            log::info("coop: presenting host P{} as local slot {}", row.key, slot);
        }
        auto* p = core::pawn(c.slot);
        if (!p) continue;
        if (auto at = core::location(p); at && (std::fabs(at->x - row.x) + std::fabs(at->y - row.y) + std::fabs(at->z - row.z)) > 0.5f) {
            reflect::Frame tp(s.actor_teleport);
            tp.arg(0, core::Vec3{row.x, row.y, row.z}).arg_bool(1, false).arg_bool(3, true);
            tp.invoke(p);
        }
        if (auto* hp = s.zc_hp.object(p)) { // presented vitals (host values; the copy never settles)
            *s.hp_max.at<std::int32_t>(hp) = row.mhp;
            *s.hp_curr.at<std::int32_t>(hp) = row.hp;
            if (c.hp >= 0 && row.hp < c.hp && row.alive) hit_react(p, c.hp - row.hp);
            c.hp = row.hp;
        }
        if (auto* mp = s.zc_mp.object(p)) {
            *s.mp_max.at<std::int32_t>(mp) = row.mmp;
            *s.mp_curr.at<std::int32_t>(mp) = row.mp;
        }
        if (row.facing && row.facing != c.facing && g_set_rotation.ok()) {
            reflect::Frame fr(g_set_rotation);
            fr.arg(0, core::Vec3{0, row.facing > 0 ? 90.0f : -90.0f, 0}).arg_bool(1, false);
            if (fr.invoke(p)) c.facing = row.facing;
        }
        if (row.anim && row.anim != c.anim && bp)
            if (const auto* anim = g_names.text(row.anim))
                if (auto* spine = bp->lily_spine.object(p)) {
                    reflect::Frame fa(s.sp_set_animation);
                    fa.arg(0, 0).arg_string(1, log::wide(*anim)).arg_bool(2, row.loop != 0);
                    if (fa.invoke(spine)) c.anim = row.anim;
                }
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
std::unordered_map<std::uint64_t, ObjHandle> g_ab_live;  // kind << 48 | id << 32 | act seq → replayed ability
std::unordered_map<std::uint32_t, ObjHandle> g_fx_live;  // host FX seq → replayed PSC
std::uint64_t g_ev_absent = 0, g_ev_retried = 0, g_snd_far = 0, g_fx_bad = 0, g_snd_bad = 0, g_stop_missed = 0;

bool world_ready() {
    bool loading = true, ready = false;
    return lifecycle::state() == lifecycle::State::Settled && world::loader_flags(loading, ready) && !loading && ready;
}

std::uint64_t ab_key(std::uint8_t kind, std::uint16_t id, std::uint32_t seq) { return static_cast<std::uint64_t>(kind) << 48 | static_cast<std::uint64_t>(id) << 32 | seq; }

EvResult handle_event(std::span<const std::uint8_t> msg) {
    switch (proto::type_of(msg)) {
    case proto::Msg::Act: {
        proto::Act a;
        if (!proto::decode(msg, a)) return ++g_act_bad, EvResult::Done;
        auto& last = g_act_last[static_cast<std::uint32_t>(a.kind) << 16 | a.net_id];
        if (a.seq <= last) return EvResult::Done; // idempotent per owner
        const auto* cls = g_names.text(a.ability);
        const auto* cs = a.custom_spawn ? g_names.text(a.custom_spawn) : nullptr;
        const auto* ca = a.custom_attach ? g_names.text(a.custom_attach) : nullptr;
        if (!cls) return ++g_act_bad, EvResult::Done;
        auto* owner = actor_of(a.kind, a.net_id);
        if (!owner) return EvResult::Retry;
        last = a.seq;
        std::string why;
        ue::UObject* spawned = nullptr;
        const auto r0 = clock::now_us();
        const bool ok = ability::replay(owner, a, *cls, cs ? *cs : "", ca ? *ca : "", &why, &spawned);
        h_replay.add(static_cast<double>(clock::now_us() - r0));
        if (!ok && ++g_act_bad <= 5) log::warn("coop: act {} on kind {} id {} rejected: {}", *cls, a.kind, a.net_id, why);
        if (spawned) g_ab_live[ab_key(a.kind, a.net_id, a.seq)] = ObjHandle::of(spawned);
        if (g_ab_live.size() > 512) std::erase_if(g_ab_live, [](const auto& kv) { return !kv.second.get(); });
        return EvResult::Done;
    }
    case proto::Msg::Fx: {
        proto::Fx m;
        if (!proto::decode(msg, m)) return ++g_fx_bad, EvResult::Done;
        const auto* ps = g_names.text(m.ps);
        const auto* bone = m.custom_bone ? g_names.text(m.custom_bone) : nullptr;
        if (!ps) return ++g_fx_bad, EvResult::Done;
        auto* owner = actor_of(m.kind, m.net_id);
        if (!owner) return EvResult::Retry;
        std::string why;
        if (auto* psc = fx::replay_spawn(owner, m, *ps, bone ? *bone : "", &why)) g_fx_live[m.seq] = ObjHandle::of(psc);
        else if (++g_fx_bad <= 5) log::warn("coop: fx {} rejected: {}", *ps, why);
        if (g_fx_live.size() > 512) std::erase_if(g_fx_live, [](const auto& kv) { return !kv.second.get(); });
        return EvResult::Done;
    }
    case proto::Msg::FxStop: {
        proto::FxStop m;
        if (!proto::decode(msg, m)) return EvResult::Done;
        auto it = g_fx_live.find(m.seq);
        if (it == g_fx_live.end()) return EvResult::Done; // never replayed here (other map / own seat / dropped)
        fx::deactivate(it->second.get());
        g_fx_live.erase(it);
        return EvResult::Done;
    }
    case proto::Msg::Snd: {
        proto::Snd m;
        if (!proto::decode(msg, m)) return ++g_snd_bad, EvResult::Done;
        const auto* ev = g_names.text(m.event);
        auto* owner = actor_of(m.kind, m.net_id);
        if (!ev) return ++g_snd_bad, EvResult::Done;
        if (!owner) return EvResult::Retry;
        const auto me = core::location(world::p1_pawn());
        const auto at = core::location(owner);
        if (me && at && std::fabs(me->y - at->y) + std::fabs(me->z - at->z) > kSoundRangeUU) return ++g_snd_far, EvResult::Done;
        std::string why;
        if (!fx::play_sound(owner, *ev, m.is2d != 0, &why) && ++g_snd_bad <= 5) log::warn("coop: sound {} rejected: {}", *ev, why);
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
        fx::stop_ability(it->second.get(), m.reason);
        g_ab_live.erase(it);
        return EvResult::Done;
    }
    default: return EvResult::Done;
    }
}

void retry_events(std::int64_t now_us) {
    for (std::size_t n = g_ev_retry.size(); n && !g_ev_retry.empty(); --n) {
        auto e = std::move(g_ev_retry.front());
        g_ev_retry.pop_front();
        if (now_us > e.deadline) {
            ++g_ev_absent;
            continue;
        }
        if (handle_event(e.msg) == EvResult::Retry) g_ev_retry.push_back(std::move(e));
        else ++g_ev_retried;
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
        root.set("build", core::profile::to_json(b));
    }
    std::vector<std::uint8_t> big(proto::kMaxJson + 64);
    if (const auto n = proto::encode(proto::Msg::Build, proto::Json{json::dump(root, 0)}, big)) coop::send_reliable(0, std::span(big.data(), n));
}

void on_reliable(std::uint32_t, std::span<const std::uint8_t> msg) {
    switch (proto::type_of(msg)) {
    case proto::Msg::Welcome: {
        proto::Welcome w;
        if (!proto::decode(msg, w)) return;
        g_seat = w.seat, g_epoch = w.epoch, g_host_map = w.map;
        g_seat_placed = false;
        if (!guard::arm()) coop::leave("guard unavailable (fail closed)");
        core::permission::p1_presentation(true);
        if (!fx::client_enable()) log::warn("coop: local FX suppression unavailable (SpawnFX hook)");
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
        if (!proto::decode(msg, m)) return;
        g_host_map = m.map;
        g_follow_tag = m.tag;
        g_follow_pending = true;
        g_follow_next_us = 0;
        break;
    }
    case proto::Msg::Act:
    case proto::Msg::Fx:
    case proto::Msg::FxStop:
    case proto::Msg::Snd:
    case proto::Msg::ActStop: {
        // after leave (stop() ran in this same session tick, enemies released, OpenLevel queued) the inbox
        // can still hold events: never replay outside a seated session (AV 0x130 in LaunchAbilityFromDescription)
        if (!g_seat) return;
        // the local world is loading / tearing down (an act replayed while the client went back to the title
        // crashed inside LaunchAbilityFromDescription: AV 0x130)
        if (!world_ready()) return void(++g_act_absent);
        if (handle_event(msg) == EvResult::Retry) {
            if (g_ev_retry.size() < 64) g_ev_retry.push_back({clock::now_us() + kRetryUs, std::vector<std::uint8_t>(msg.begin(), msg.end())});
            else ++g_ev_absent;
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
        coop::send_reliable(0, std::span(g_buf.data(), proto::encode(proto::MapReady{g_host_map}, g_buf)));
        g_follow_pending = false;
        g_seat_placed = false;
        g_taken.clear(); // the previous map's enemies are gone (unload ≠ death)
        for (auto* a : adapters()) a->reset();
        return;
    }
    g_follow_next_us = now_us + 2'000'000; // ≥ 2 s between attempts
    drop_copies();
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
            freeze(e);
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
    if (g_equip_sent.empty()) return void(g_equip_sent = cur); // the Build already carried this one
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
    const double render = static_cast<double>(now_us) / 1000.0 + g_offset_ms - 100.0;
    // a position write costs ~80 µs (actor + Spine children transform update, net-facts「敌人位置插值」):
    // enemies farther than 3000 UU from the local lily (off-screen) are written at ~10 Hz only
    static std::uint32_t frame = 0;
    ++frame;
    const auto me = core::location(world::p1_pawn());
    int idx = 0;
    for (auto& [n, t] : g_taken) {
        ++idx;
        if (!t.n || t.hidden) continue;
        auto* e = t.enemy.get();
        if (!e) continue;
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

void tick(std::int64_t now_us) {
    if (!g_seat || lifecycle::state() != lifecycle::State::Settled) return;
    auto t0 = clock::now_us();
    present_motion(now_us);
    auto t1 = clock::now_us();
    h_motion.add(static_cast<double>(t1 - t0));
    equip_poll(now_us);
    auto t2 = clock::now_us();
    send_input();
    follow(now_us);
    auto t3 = clock::now_us();
    h_input.add(static_cast<double>(t3 - t2));
    recheck(now_us);
    retry_events(now_us);
    sync::rec_tick();
    if (t2 - t1 + clock::now_us() - t3 > 50) h_slow.add(static_cast<double>(t2 - t1 + clock::now_us() - t3));
}

void stop() {
    const bool was_seated = g_seat != 0;
    drop_copies();
    for (auto& [name, t] : g_taken)
        if (auto* e = t.enemy.get(); e && g_set_tick.ok()) {
            guard::unmark(e);
            reflect::Frame f(g_set_tick);
            f.arg_bool(0, true);
            f.invoke(e);
        }
    g_taken.clear();
    guard::disarm();
    core::permission::p1_presentation(false);
    overlay::set_banner({});
    g_seat = 0, g_seat_placed = false, g_epoch = 0, g_latest = 0;
    g_ring.reset();
    g_names.clear();
    g_act_last.clear();
    g_ev_retry.clear(), g_ab_live.clear(), g_fx_live.clear();
    fx::client_disable();
    reset_spirits();
    for (auto* a : adapters()) a->reset();
    g_equip_sent.clear();
    g_index.clear();
    g_index_version = ~0ull;
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

void present_dynamic(const ClientCtx& c) {
    const auto* s = core::schema::script();
    if (!s || !g_resolved) return;
    for (auto& [k, d] : g_dyn) d.seen = false;
    for (const auto& row : c.frame.ed.view()) {
        auto& d = g_dyn[row.key];
        d.seen = true;
        auto* e = d.actor.get();
        if (!e) {
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
            guard::unmark(e);
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

// adapter spirit (client half, P6): one presentation actor per host spirit row of another seat — spawned from
// the row's class, frozen (presentation component whitelist), marked, no collision; shown / hidden with the
// row (kept for reuse), positioned / turned / posed from the row like the lily copies.
struct SpiritCopy {
    ObjHandle actor, spine;
    std::uint16_t cls = 0, anim = 0;
    std::int8_t facing = 0;
    bool seen = false, visible = false;
};
std::map<std::uint16_t, SpiritCopy> g_spirits;
std::uint64_t g_spirit_spawned = 0, g_spirit_failed = 0;

namespace {
void set_hidden(ue::UObject* a, bool hidden) {
    static reflect::FuncRef hide = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"SetActorHiddenInGame", 1);
    if (!a || !hide.ok()) return;
    reflect::Frame h(hide);
    h.arg_bool(0, hidden);
    h.invoke(a);
}
} // namespace

void present_spirits(const ClientCtx& c) {
    const auto* s = core::schema::script();
    if (!s || !g_seat) return;
    if (!g_set_collision.ok()) {
        reflect::Resolver a("Actor", reflect::find_class(L"/Script/Engine.Actor"));
        g_set_collision = a.func(L"SetActorEnableCollision", 1);
        g_set_rotation = a.func(L"K2_SetActorRotation", 2);
    }
    static reflect::PropRef loco_prop =
        reflect::Resolver("ZenithSpirit", reflect::find_class(L"/Script/Zenith.ZenithSpirit")).prop(L"LocomotionComponent", reflect::Kind::Object);
    static reflect::FuncRef set_facing = reflect::Resolver("LocomotionComponent", reflect::find_class(L"/Script/Zenith.LocomotionComponent")).func(L"SetFacing", 1);
    static reflect::FuncRef set_time = reflect::Resolver("TrackEntry", reflect::find_class(L"/Script/SpinePlugin.TrackEntry")).func(L"SetTrackTime", 1);
    static ObjHandle spine_cls = ObjHandle::of(reflect::find_object(L"/Script/SpineExtensionPlugin.SpineSkeletonAnimationExComponent"));
    for (auto& [k, sc] : g_spirits) sc.seen = false;
    for (const auto& row : c.frame.sp.view()) {
        if ((row.key >> 8) == g_seat) continue; // own spirits: the local P1 summons them itself
        auto& sc = g_spirits[row.key];
        sc.seen = true;
        auto* a = sc.actor.get();
        if (!a || sc.cls != row.cls) {
            if (a) guard::unmark(a), entities::destroy_copy(a);
            sc = SpiritCopy{};
            sc.seen = true;
            const auto* path = c.text(row.cls);
            auto* cls = path ? entities::class_by_path(*path) : nullptr;
            static ue::UClass* spirit_base = reflect::find_class(L"/Script/Zenith.ZenithSpirit");
            if (!cls || !spirit_base || !cls->IsChildOf(spirit_base)) continue;
            if (!(a = entities::spawn_copy(cls, row.x, row.y, row.z))) {
                ++g_spirit_failed;
                continue;
            }
            ++g_spirit_spawned;
            sc.actor = ObjHandle::of(a);
            sc.cls = row.cls;
            if (g_set_collision.ok()) {
                reflect::Frame fc(g_set_collision);
                fc.arg_bool(0, false);
                fc.invoke(a);
            }
            freeze(a);
            guard::mark(a);
            reflect::Frame sf(s->actor_component_by_class);
            sf.arg(0, spine_cls.get());
            if (sf.invoke(a)) sc.spine = ObjHandle::of(sf.ret<ue::UObject*>());
        }
        if (!sc.visible) set_hidden(a, false), sc.visible = true;
        if (auto at = core::location(a); at && (std::fabs(at->x - row.x) + std::fabs(at->y - row.y) + std::fabs(at->z - row.z)) > 0.5f) {
            reflect::Frame tp(s->actor_teleport);
            tp.arg(0, core::Vec3{row.x, row.y, row.z}).arg_bool(1, false).arg_bool(3, false);
            tp.invoke(a);
        }
        if (row.facing && row.facing != sc.facing)
            if (auto* loco = loco_prop.ok() ? loco_prop.object(a) : nullptr; loco && set_facing.ok()) {
                reflect::Frame ff(set_facing);
                ff.arg_bool(0, row.facing > 0);
                if (ff.invoke(loco)) sc.facing = row.facing;
            }
        if (row.anim && row.anim != sc.anim)
            if (const auto* anim = g_names.text(row.anim); anim && sc.spine.get()) {
                reflect::Frame f(s->sp_set_animation);
                f.arg(0, 0).arg_string(1, log::wide(*anim)).arg_bool(2, row.loop != 0);
                if (f.invoke(sc.spine.get())) {
                    if (auto* entry = f.ret<ue::UObject*>(); set_time.ok() && ue::alive(entry) && row.t > 0) {
                        reflect::Frame st(set_time);
                        st.arg(0, row.t);
                        st.invoke(entry);
                    }
                    sc.anim = row.anim;
                }
            }
    }
    for (auto& [k, sc] : g_spirits)
        if (!sc.seen && sc.visible) set_hidden(sc.actor.get(), true), sc.visible = false, sc.anim = 0;
}

void reset_spirits() {
    for (auto& [k, sc] : g_spirits)
        if (auto* a = sc.actor.get()) guard::unmark(a), entities::destroy_copy(a);
    g_spirits.clear();
}

ue::UObject* actor_of(std::uint8_t kind, std::uint16_t id) {
    ue::UObject* a = nullptr;
    switch (static_cast<proto::RefKind>(kind)) {
    case proto::RefKind::Enemy: {
        // only enemies we have taken over and still present (a hidden / host-dead copy, or one not taken
        // yet, has no valid presentation state for an ability)
        const auto* name = g_names.text(id);
        auto t = name ? g_taken.find(*name) : g_taken.end();
        if (t != g_taken.end() && !t->second.hidden && !t->second.killed) a = t->second.enemy.get();
        break;
    }
    case proto::RefKind::Dyn: {
        auto t = g_taken.find(std::format("dyn:{}", id));
        if (t != g_taken.end() && !t->second.hidden && !t->second.killed) a = t->second.enemy.get();
        break;
    }
    case proto::RefKind::Lily:
        if (auto it = g_copies.find(id); it != g_copies.end()) a = core::pawn(it->second.slot);
        break;
    case proto::RefKind::Spirit:
        if (auto it = g_spirits.find(id); it != g_spirits.end() && it->second.visible) a = it->second.actor.get();
        break;
    }
    return a && !a->HasAnyFlags(RC::Unreal::RF_BeginDestroyed) ? a : nullptr;
}

std::string describe_events() {
    return std::format("events(client): hitReacts={} spirits={} spawned={} failed={} retryQueue={} retried={} absent={} fxBad={} soundBad={} soundFar={} stopMissed={} live abilities={} fx={}\n",
                       g_hit_reacts, g_spirits.size(), g_spirit_spawned, g_spirit_failed, g_ev_retry.size(), g_ev_retried, g_ev_absent, g_fx_bad, g_snd_bad, g_snd_far, g_stop_missed, g_ab_live.size(),
                       g_fx_live.size());
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
    return std::format("client: seat=P{} epoch={} latest={} names={} taken={} applied={} badSnaps={} unknownNames={} animSets={} animFail={} copies={} reconciles={} kills={} actBad={} actAbsent={} rechecks={} aiLeaks={} world={} equipRequests={} motionWrites={} presses(attack)={}\n{}{}", g_seat,
                       g_epoch, g_latest, g_names.size(), g_taken.size(), g_applied, g_bad_snaps, g_unknown_names, g_anim_sets, g_anim_fail, g_copies.size(), g_reconciles, g_kills, g_act_bad, g_act_absent, g_rechecks, g_leaks, std::uint64_t{0}, g_equip_requests, g_motion_writes, g_input.presses[3], ability::describe() + fx::describe() + describe_events() + adapters_text(),
                       guard::describe());
}

// `elmp lan sync` (coop/sync.h): what this client presents, keyed like the host's report
namespace {
std::string sync_spirit_key(ue::UObject* a) {
    static reflect::PropRef owner = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).prop(L"Owner", reflect::Kind::Object);
    const auto cls = log::utf8(ue::class_name_of(a));
    auto* p1c = world::p1_controller();
    if (owner.ok() && p1c && owner.object(a) == p1c) return std::format("s:{}:{}", g_seat, cls);
    for (const auto& [k, sc] : g_spirits)
        if (sc.actor.get() == a) return std::format("s:{}:{}", k >> 8, cls);
    return {};
}

std::string sync_key_of(ue::UObject* a) {
    static ue::UClass* spirit = reflect::find_class(L"/Script/Zenith.ZenithSpirit");
    static ue::UClass* zc = reflect::find_class(L"/Script/Zenith.ZenithCharacter");
    if (a == world::p1_pawn()) return std::format("l:{}", g_seat);
    for (const auto& [k, c] : g_copies)
        if (core::pawn(c.slot) == a) return std::format("l:{}", k);
    if (spirit && a->GetClassPrivate()->IsChildOf(spirit)) return sync_spirit_key(a);
    if (!zc || !a->GetClassPrivate()->IsChildOf(zc) || !g_resolved) return {};
    if (auto* sp = g_spawn_point.object(a)) return "e:" + ue::utf8_name(sp);
    for (const auto& [k, d] : g_dyn)
        if (d.actor.get() == a) return std::format("d:{}", k);
    return {};
}

std::vector<sync::Ent> sync_entities() {
    resolve();
    std::vector<sync::Ent> v;
    static reflect::PropRef hidden = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).prop(L"bHidden", reflect::Kind::Bool);
    if (auto* p1 = world::p1_pawn()) v.push_back({std::format("l:{}", g_seat), p1, true});
    for (const auto& [k, c] : g_copies)
        if (auto* p = core::pawn(c.slot)) v.push_back({std::format("l:{}", k), p, true});
    for (const auto& e : registry::bucket(registry::Kind::Enemy))
        if (auto* a = e.handle.get(); a && g_resolved) {
            const auto k = sync_key_of(a);
            v.push_back({k.empty() ? "x:" + ue::utf8_name(a) : k, a, false});
        }
    for (const auto& e : registry::bucket(registry::Kind::Spirit))
        if (auto* a = e.handle.get(); a && hidden.ok() && !hidden.read_bool(a)) {
            const auto k = sync_spirit_key(a);
            v.push_back({k.empty() ? "s:?:" + log::utf8(ue::class_name_of(a)) : k, a, false});
        }
    return v;
}
} // namespace

std::string rec(const std::string& sub, int frames) { return sub == "start" ? sync::rec_start(sync_entities, frames) : sync::rec_dump(); }

std::string sync_report(const std::string& cat) {
    std::string out;
    sync::header(out, "client", g_seat, std::format("\"latest\":{}", g_latest));
    if (sync::wants(cat, "ent"))
        for (const auto& e : sync_entities()) sync::entity(out, e);
    if (sync::wants(cat, "fx")) sync::fx(out, sync_key_of);
    if (sync::wants(cat, "cnt")) {
        sync::guard_counters(out);
        out += std::format("{{\"cat\":\"cnt\",\"src\":\"client\",\"applied\":{},\"bad_snaps\":{},\"unknown_names\":{},\"taken\":{},\"kills\":{},\"copies\":{},\"spirits\":{},"
                           "\"reconciles\":{},\"anim_sets\":{},\"anim_fail\":{},\"act_bad\":{},\"act_absent\":{},\"hit_reacts\":{},\"ev_retried\":{},\"ev_absent\":{},"
                           "\"fx_bad\":{},\"sound_bad\":{},\"sound_far\":{},\"stop_missed\":{},\"ai_leaks\":{},\"dyn_bound\":{}}}\n",
                           g_applied, g_bad_snaps, g_unknown_names, g_taken.size(), g_kills, g_copies.size(), g_spirits.size(), g_reconciles, g_anim_sets, g_anim_fail, g_act_bad,
                           g_act_absent, g_hit_reacts, g_ev_retried, g_ev_absent, g_fx_bad, g_snd_bad, g_snd_far, g_stop_missed, g_leaks, g_dyn.size());
    }
    if (sync::wants(cat, "world")) sync::world_objects(out);
    if (sync::wants(cat, "hash")) sync::hashes(out);
    return out;
}

void install() { coop::set_handlers(Role::Client, {on_connected, on_closed, on_reliable, on_unreliable, tick, stop}); }

} // namespace elmp::coop::client
