// Coop client (first slice): guest key → Build; Welcome arms the guard; every frame the local P1 input
// snapshot goes upstream (axes, held bits, cumulative press counters, snapshot ack); snapshots are decoded
// against the acknowledged base; enemies named by their spawn point are taken over (guard mark on pawn +
// controller, actor tick off, CanBePaused untouched by vanilla since activation is gated) and placed /
// HP-written from the host rows. The client process never settles (guard), never saves (guard).

#include "coop/client.h"

#include "coop/commands.h"
#include "overlay/render.h"
#include "coop/entities/clearable.h"
#include "coop/entities/spawn.h"
#include "coop/entity.h"
#include "coop/session.h"

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
#include "input/virtual_pad.h"

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
constexpr float kCmdCorrectUU = 300.0f; // enemy running a replayed command: vanilla Move owns position below this error
constexpr float kCopySnapUU = 240.0f, kCopyBlendUU = 40.0f; // lily copy vs host seat (v1 values, to be re-measured)
std::uint64_t g_copy_snaps = 0, g_copy_blends = 0;
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
    ObjHandle queue, move;   // CommandQueueComponent / CharacterMovement (ADR 0006 position ownership)
    float lx = 0, ly = 0, lz = 0; // last written position (velocity feed)
    std::int64_t lus = 0;
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
std::unordered_map<std::uint32_t, std::uint32_t> g_cmd_last; // kind << 16 | owner id → last cmd seq
std::uint64_t g_cmd_bad = 0;
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

// ADR 0006 takeover: stop the decision source only. The AI controller stays gated (guard: no Activate, no
// behaviour tree); the pawn's execution layer runs vanilla — actor + every component ticking, world
// collision on — so a replayed command plays its State / Ability / Move / FX / Sound timelines. The guard
// keeps the pawn ticking (the disabling direction of SetActorTickEnabled is refused for marked pawns: a
// dormant enemy would not sample the host's command) and the settlement boundary stops every result.
void run_exec(ue::UObject* enemy) {
    static reflect::FuncRef set_comp_tick;
    static ObjHandle comp_cls;
    if (!set_comp_tick.ok()) {
        set_comp_tick = reflect::Resolver("ActorComponent", reflect::find_class(L"/Script/Engine.ActorComponent")).func(L"SetComponentTickEnabled", 1);
        comp_cls = ObjHandle::of(reflect::find_object(L"/Script/Engine.ActorComponent"));
    }
    if (!g_set_collision.ok()) g_set_collision = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"SetActorEnableCollision", 1);
    const auto* s = core::schema::script();
    if (s && set_comp_tick.ok()) {
        reflect::Frame f(s->actor_components_by_class);
        f.arg(0, comp_cls.get());
        if (f.invoke(enemy))
            if (const auto* arr = static_cast<const reflect::RawArray*>(f.raw(s->actor_components_by_class.ret)); arr && arr->num >= 0 && arr->num <= 128) {
                const auto* items = reinterpret_cast<ue::UObject* const*>(arr->data);
                for (int i = 0; i < arr->num; ++i) {
                    if (!ue::alive(items[i])) continue;
                    reflect::Frame on(set_comp_tick);
                    on.arg_bool(0, true);
                    on.invoke(items[i]);
                }
            }
    }
    for (auto* fn : {&g_set_tick, &g_set_collision})
        if (fn->ok()) {
            reflect::Frame f(*fn);
            f.arg_bool(0, true);
            f.invoke(enemy);
        }
}

void take(const std::string& name, ue::UObject* enemy) {
    auto* controller = core::schema::script() ? core::schema::script()->pawn_controller.object(enemy) : nullptr;
    guard::mark(enemy);
    guard::mark(controller);
    guard::mark_decision(controller);
    run_exec(enemy);
    guard::mark_exec(enemy);
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
        if ((row.flags & 4) && !t.killed) {
            // the vanilla death process is a settlement (drops, spawn point clear) and is never run here: the
            // corpse stops its execution layer and shows the host's death pose from the rows
            t.killed = true, ++g_kills;
            guard::mark_exec(enemy, false);
            freeze(enemy);
            t.anim = 0;
        }
    }
    t.frame = g_latest;
    // ADR 0006: the vanilla execution layer animates a live enemy (commands, locomotion state machine); the
    // host pose is applied only to a dead one
    if (t.killed && row.anim && row.anim != t.anim) { // pose: switch track 0 only on an animation change (§16)
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
    std::int64_t mismatch_us = 0; // since when the copy's own track 0 differs from the host's (0 = agrees)
    bool held = false;            // host pose held: the copy's animation logic is paused (non-replicated state)
};
std::map<std::uint16_t, Copy> g_copies;
std::uint64_t g_pose_holds = 0;

// ADR 0006 §7.21-6 fallback: states the host reaches without a replicated command (chair rest / sleep —
// INTERACT is settlement and never replayed — cutscene poses, etc.) cannot come from the copy's own
// pipeline. When the copy is idle (no running command, no input) and its track 0 has disagreed with the
// host's for > 300 ms, the copy's animation logic (SpineAnimationComponent, which rewrites track 0 every
// frame) is paused and the host pose is applied from the rows; any command or input releases it.
void hold_host_pose(ue::UObject* p, Copy& c, const snap::LilyRow& row, const core::schema::Script& s, const core::schema::Blueprint* bp) {
    static reflect::PropRef zc_queue, cq_array;
    static reflect::FuncRef comp_tick;
    static ObjHandle logic_cls;
    if (!cq_array.ok()) {
        zc_queue = reflect::Resolver("ZenithCharacter", reflect::find_class(L"/Script/Zenith.ZenithCharacter")).prop(L"CommandQueueComponent", reflect::Kind::Object);
        cq_array = reflect::Resolver("CommandQueueComponent", reflect::find_class(L"/Script/Zenith.CommandQueueComponent")).prop(L"CommandQueue", reflect::Kind::Array);
        comp_tick = reflect::Resolver("ActorComponent", reflect::find_class(L"/Script/Engine.ActorComponent")).func(L"SetComponentTickEnabled", 1);
        logic_cls = ObjHandle::of(reflect::find_object(L"/Script/Zenith.SpineAnimationComponent"));
    }
    auto* spine = bp ? bp->lily_spine.object(p) : nullptr;
    const auto* host_anim = row.anim ? g_names.text(row.anim) : nullptr;
    if (!spine || !host_anim || !cq_array.ok() || !comp_tick.ok()) return;
    auto* queue = zc_queue.object(p);
    const bool busy = (queue && cq_array.at<reflect::RawArray>(queue)->num > 0) || row.ix || row.iy || row.iraw;
    auto set_logic = [&](bool on) {
        reflect::Frame f(s.actor_component_by_class);
        f.arg(0, logic_cls.get());
        if (auto* logic = f.invoke(p) ? f.ret<ue::UObject*>() : nullptr) {
            reflect::Frame t(comp_tick);
            t.arg_bool(0, on);
            t.invoke(logic);
        }
    };
    if (busy) {
        if (c.held) set_logic(true), c.held = false;
        c.mismatch_us = 0;
        return;
    }
    std::wstring own;
    reflect::Frame cur(s.sp_get_current);
    cur.arg(0, 0);
    if (cur.invoke(spine))
        if (auto* entry = cur.ret<ue::UObject*>(); ue::alive(entry)) {
            reflect::Frame nm(s.te_animation_name);
            if (nm.invoke(entry)) own = nm.ret_string();
        }
    const auto now = clock::now_us();
    if (own == log::wide(*host_anim)) {
        c.mismatch_us = 0;
        return;
    }
    if (!c.held) {
        if (!c.mismatch_us) c.mismatch_us = now;
        if (now - c.mismatch_us < 300'000) return;
        set_logic(false);
        c.held = true, c.anim = 0;
        ++g_pose_holds;
    }
    if (row.anim != c.anim || own.empty()) {
        reflect::Frame fa(s.sp_set_animation);
        fa.arg(0, 0).arg_string(1, log::wide(*host_anim)).arg_bool(2, row.loop != 0);
        if (fa.invoke(spine)) {
            c.anim = row.anim;
            static reflect::FuncRef set_time = reflect::Resolver("TrackEntry", reflect::find_class(L"/Script/SpinePlugin.TrackEntry")).func(L"SetTrackTime", 1);
            if (auto* entry = fa.ret<ue::UObject*>(); set_time.ok() && ue::alive(entry) && row.t > 0) {
                reflect::Frame st(set_time);
                st.arg(0, row.t);
                st.invoke(entry);
            }
        }
    }
}

void drop_copy(std::uint16_t key) {
    auto it = g_copies.find(key);
    if (it == g_copies.end()) return;
    if (auto* p = core::pawn(it->second.slot)) guard::unmark(p), guard::mark_not_player(p, false), commands::suppress_input_commands(p, false);
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
            resolve();
            // ADR 0006: the copy runs the vanilla pipeline (nothing frozen, world collision on); its input is the
            // host seat's (virtual pad, below), its commands come only from the host (input-made ones suppressed)
            guard::mark(p);
            guard::mark_not_player(p, true);
            guard::mark_exec(p);
            if (!commands::suppress_input_commands(p, true)) log::warn("coop: copy P{}: input command suppression unavailable", row.key);
            if (g_set_collision.ok()) {
                reflect::Frame fc(g_set_collision);
                fc.arg_bool(0, true);
                fc.invoke(p);
            }
            c.anim = 0, c.facing = 0;
            log::info("coop: presenting host P{} as local slot {}", row.key, slot);
        }
        auto* p = core::pawn(c.slot);
        if (!p) continue;
        input::pad::set(c.slot, row.ix / 100.0, row.iy / 100.0, row.iraw);
        // position correction (ADR 0006 §7.21-5): > 240 UU jump, 40–240 converge 15 % per snapshot, below: vanilla
        if (auto at = core::location(p)) {
            const float dx = row.x - at->x, dy = row.y - at->y, dz = row.z - at->z;
            const float err = std::fabs(dy) + std::fabs(dz);
            if (err > kCopySnapUU || err > kCopyBlendUU) {
                const float k = err > kCopySnapUU ? 1.0f : 0.15f;
                reflect::Frame tp(s.actor_teleport);
                tp.arg(0, core::Vec3{at->x + dx * k, at->y + dy * k, at->z + dz * k}).arg_bool(1, false).arg_bool(3, false);
                if (tp.invoke(p)) ++(k == 1.0f ? g_copy_snaps : g_copy_blends);
            }
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
        // facing / animation come from the copy's own vanilla pipeline (input + host commands); the host facing
        // is applied only while the copy stands still and disagrees (e.g. a turn made by a command)
        if (row.facing && row.ix == 0 && g_set_rotation.ok() && s.zc_locomotion.ok())
            if (auto* loco = s.zc_locomotion.object(p)) {
                reflect::Frame ff(s.loco_facing_right);
                if (ff.invoke(loco) && (ff.ret_bool() ? 1 : -1) != row.facing) {
                    static reflect::FuncRef set_facing =
                        reflect::Resolver("LocomotionComponent", reflect::find_class(L"/Script/Zenith.LocomotionComponent")).func(L"SetFacing", 1);
                    if (set_facing.ok()) {
                        reflect::Frame sf(set_facing);
                        sf.arg_bool(0, row.facing > 0);
                        sf.invoke(loco);
                    }
                }
            }
        hold_host_pose(p, c, row, s, bp);
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
enum class EvResult { Done, Retry };
struct RetryEv {
    std::int64_t deadline = 0;
    std::vector<std::uint8_t> msg;
};
std::deque<RetryEv> g_ev_retry;
std::uint64_t g_ev_absent = 0, g_ev_retried = 0;

bool world_ready() {
    bool loading = true, ready = false;
    return lifecycle::state() == lifecycle::State::Settled && world::loader_flags(loading, ready) && !loading && ready;
}


EvResult handle_event(std::span<const std::uint8_t> msg) {
    switch (proto::type_of(msg)) {
    case proto::Msg::Cmd: { // ADR 0006: the host's decision, run through the vanilla command queue
        proto::Cmd m;
        if (!proto::decode(msg, m)) return ++g_cmd_bad, EvResult::Done;
        auto& last = g_cmd_last[static_cast<std::uint32_t>(m.kind) << 16 | m.net_id];
        if (m.seq <= last) return EvResult::Done; // idempotent per owner
        const auto* cls = g_names.text(m.cls);
        if (!cls) return ++g_cmd_bad, EvResult::Done;
        auto* owner = actor_of(m.kind, m.net_id);
        if (!owner) return EvResult::Retry;
        last = m.seq;
        std::string why;
        const auto r0 = clock::now_us();
        const bool ok = commands::replay(owner, *cls, m.dir, m.cmd, &why);
        h_replay.add(static_cast<double>(clock::now_us() - r0));
        if (!ok && ++g_cmd_bad <= 5) log::warn("coop: cmd {} on kind {} id {} rejected: {}", *cls, m.kind, m.net_id, why);
        return EvResult::Done;
    }
    default: return EvResult::Done;
    }
}

void queue_event(std::span<const std::uint8_t> msg) {
    if (handle_event(msg) == EvResult::Retry) {
        if (g_ev_retry.size() < 64) g_ev_retry.push_back({clock::now_us() + kRetryUs, std::vector<std::uint8_t>(msg.begin(), msg.end())});
        else ++g_ev_absent;
    }
}

// Enemy commands run on the interpolation timeline (host time − 100 ms, the same time the presented
// position shows) so the vanilla action starts where the enemy is drawn; lily copies run live (they are
// driven by live input, P7-4).
struct DueCmd {
    std::uint32_t t_ms = 0;
    std::vector<std::uint8_t> msg;
};
std::deque<DueCmd> g_cmd_due;
double render_host_ms(std::int64_t now_us);

void run_due_cmds(std::int64_t now_us) {
    const double render = render_host_ms(now_us);
    while (!g_cmd_due.empty() && (render >= g_cmd_due.front().t_ms || g_cmd_due.size() > 192)) {
        queue_event(g_cmd_due.front().msg);
        g_cmd_due.pop_front();
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
    case proto::Msg::Cmd: {
        if (!g_seat || !world_ready()) return void(++g_act_absent);
        proto::Cmd m;
        if (!proto::decode(msg, m)) return void(++g_cmd_bad);
        const auto kind = static_cast<proto::RefKind>(m.kind);
        if (kind == proto::RefKind::Enemy || kind == proto::RefKind::Dyn) {
            if (g_cmd_due.size() < 256) g_cmd_due.push_back({m.t_ms, std::vector<std::uint8_t>(msg.begin(), msg.end())});
            else ++g_ev_absent;
        } else {
            queue_event(msg);
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
            guard::mark_exec(e, false); // fail closed: an AI that ran is stopped with the whole actor
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

double render_host_ms(std::int64_t now_us) { return g_offset_known ? static_cast<double>(now_us) / 1000.0 + g_offset_ms - 100.0 : 0.0; }

void present_motion(std::int64_t now_us) {
    const auto* s = core::schema::script();
    if (!s || !g_offset_known) return;
    const double render = render_host_ms(now_us);
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
        // ADR 0006 position ownership: while a replayed command runs, its vanilla Move timeline moves the enemy
        // and the snapshot only corrects a large error; otherwise the interpolated position is written and its
        // velocity is fed to CharacterMovement (the vanilla animation state machine picks idle / walk / turn)
        static reflect::PropRef zc_queue, cq_array, ch_move, mv_velocity;
        if (!cq_array.ok()) {
            zc_queue = reflect::Resolver("ZenithCharacter", reflect::find_class(L"/Script/Zenith.ZenithCharacter")).prop(L"CommandQueueComponent", reflect::Kind::Object);
            cq_array = reflect::Resolver("CommandQueueComponent", reflect::find_class(L"/Script/Zenith.CommandQueueComponent")).prop(L"CommandQueue", reflect::Kind::Array);
            ch_move = reflect::Resolver("Character", reflect::find_class(L"/Script/Engine.Character")).prop(L"CharacterMovement", reflect::Kind::Object);
            mv_velocity = reflect::Resolver("MovementComponent", reflect::find_class(L"/Script/Engine.MovementComponent")).prop(L"Velocity", reflect::Kind::Struct, 12);
        }
        if (!t.queue.get() && zc_queue.ok()) t.queue = ObjHandle::of(zc_queue.object(e));
        if (!t.move.get() && ch_move.ok()) t.move = ObjHandle::of(ch_move.object(e));
        auto* queue = t.queue.get();
        const bool busy = queue && cq_array.ok() && cq_array.at<reflect::RawArray>(queue)->num > 0;
        const auto at = core::location(e);
        if (busy && at && std::fabs(at->y - p.y) + std::fabs(at->z - p.z) <= kCmdCorrectUU) {
            t.lus = 0;
            continue;
        }
        if (auto* mv = t.move.get(); mv && mv_velocity.ok() && !busy) {
            const double dt = t.lus ? (now_us - t.lus) / 1e6 : 0.0;
            core::Vec3 v{0, 0, 0};
            if (dt > 0.001 && dt < 0.25) v = {static_cast<float>((p.x - t.lx) / dt), static_cast<float>((p.y - t.ly) / dt), static_cast<float>((p.z - t.lz) / dt)};
            *mv_velocity.at<core::Vec3>(mv) = v;
        }
        t.lx = p.x, t.ly = p.y, t.lz = p.z, t.lus = now_us;
        if (at && (std::fabs(at->x - p.x) + std::fabs(at->y - p.y) + std::fabs(at->z - p.z)) > 0.2f) {
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
    run_due_cmds(now_us);
    retry_events(now_us);
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
    g_cmd_last.clear(), g_cmd_due.clear();
    commands::clear_suppressed();
    g_ev_retry.clear();
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
    case proto::RefKind::Spirit: break; // derived by the summoner's replayed command, never addressed
    }
    return a && !a->HasAnyFlags(RC::Unreal::RF_BeginDestroyed) ? a : nullptr;
}

std::string describe_events() {
    return std::format("cmds(client): due={} bad={} owners={} copySnaps={} copyBlends={} poseHolds={}\n", g_cmd_due.size(), g_cmd_bad, g_cmd_last.size(), g_copy_snaps,
                       g_copy_blends, g_pose_holds) +
           commands::describe() +
           std::format("events(client): hitReacts={} retryQueue={} retried={} absent={}\n", g_hit_reacts, g_ev_retry.size(), g_ev_retried, g_ev_absent);
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
                       g_epoch, g_latest, g_names.size(), g_taken.size(), g_applied, g_bad_snaps, g_unknown_names, g_anim_sets, g_anim_fail, g_copies.size(), g_reconciles, g_kills, g_act_bad, g_act_absent, g_rechecks, g_leaks, std::uint64_t{0}, g_equip_requests, g_motion_writes, g_input.presses[3], describe_events() + adapters_text(),
                       guard::describe());
}

void install() { coop::set_handlers(Role::Client, {on_connected, on_closed, on_reliable, on_unreliable, tick, stop}); }

} // namespace elmp::coop::client
