// Coop client (first slice): guest key → Build; Welcome arms the guard; every frame the local P1 input
// snapshot goes upstream (axes, held bits, cumulative press counters, snapshot ack); snapshots are decoded
// against the acknowledged base; enemies named by their spawn point are taken over (guard mark on pawn +
// controller, actor tick off, CanBePaused untouched by vanilla since activation is gated) and placed /
// HP-written from the host rows. The client process never settles (guard), never saves (guard).

#include "coop/client.h"

#include "coop/ability.h"
#include "coop/session.h"

#include "guard/guard.h"

#include "core/player.h"
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
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

namespace elmp::coop::client {

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
};

std::uint8_t g_seat = 0;
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
std::unordered_map<std::uint16_t, std::uint32_t> g_act_last;
metrics::Histogram& h_apply = metrics::histogram("coop.client.apply_us");
metrics::Histogram& h_motion = metrics::histogram("coop.client.motion_us");
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

// World objects (adapter worldobj): host cleared → interactables get the vanilla "already cleared" entry
// (OnClearedStatusChecked(true) — the same call the map makes for objects cleared in the save: opened door /
// chest, consumed item); host gone (breakable broken / item destroyed) → hidden + no collision. Presentation
// only: nothing is marked cleared in the client's save (and saving is refused by the guard anyway).
std::unordered_map<std::string, std::uint8_t> g_world_done; // name → flags already presented
std::uint64_t g_world_presented = 0;

void present_world(const snap::Frame& f) {
    if (!f.w.n) return;
    static reflect::FuncRef status, hide;
    if (!status.ok()) {
        status = reflect::Resolver("Interactable", reflect::find_class(L"/Script/Zenith.Interactable")).func(L"OnClearedStatusChecked", 1);
        hide = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"SetActorHiddenInGame", 1);
    }
    static ue::UClass* interactable = reflect::find_class(L"/Script/Zenith.Interactable");
    for (const auto& row : f.w.view()) {
        const auto* name = g_names.text(row.key);
        if (!name) continue;
        auto& done = g_world_done[*name];
        if ((done & row.flags) == row.flags) continue;
        ue::UObject* a = nullptr;
        for (auto kind : {registry::Kind::Interactable, registry::Kind::Breakable}) {
            for (const auto& e : registry::bucket(kind))
                if (auto* o = e.handle.get(); o && ue::utf8_name(o) == *name) a = o;
            if (a) break;
        }
        if (!a) continue; // not streamed in locally yet
        if ((row.flags & 1) && !(done & 1) && interactable && a->GetClassPrivate()->IsChildOf(interactable) && status.ok()) {
            reflect::Frame fs(status);
            fs.arg_bool(0, true);
            fs.invoke(a);
        }
        static ue::UClass* breakable = reflect::find_class(L"/Script/Zenith.Breakable");
        const bool is_breakable = breakable && a->GetClassPrivate()->IsChildOf(breakable);
        // Broken on the host → hidden + no collision (no vanilla break replay on the client: drops would be
        // spawned locally; presentation decision P2, net-facts「worldobj」).
        if ((row.flags & 2 || (is_breakable && (row.flags & 4))) && !(done & 2) && hide.ok()) {
            reflect::Frame h(hide);
            h.arg_bool(0, true);
            h.invoke(a);
            if (g_set_collision.ok()) {
                reflect::Frame fc(g_set_collision);
                fc.arg_bool(0, false);
                fc.invoke(a);
            }
        }
        done |= row.flags | ((is_breakable && (row.flags & 4)) ? 2 : 0);
        ++g_world_presented;
    }
}

void apply(const snap::Frame& f) {
    const auto t0 = clock::now_us();
    const auto* s = core::schema::script();
    resolve();
    if (!s || !g_resolved) return;
    reindex();
    for (const auto& row : f.e.view()) {
        const auto* name = g_names.text(row.key);
        if (!name) {
            ++g_unknown_names;
            continue;
        }
        auto it = g_index.find(*name);
        auto* enemy = it == g_index.end() ? nullptr : it->second.get();
        if (!enemy) continue; // not streamed in locally (Client ⊆ Host)
        if (auto t = g_taken.find(*name); t == g_taken.end() || t->second.enemy.get() != enemy) take(*name, enemy);
        auto& t = g_taken[*name];
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
    present_lilies(f, *s);
    present_world(f);
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
        if (row.key == g_seat) { // own seat: the local P1 predicts; snap back when it drifts from the host seat
            auto* p1 = world::p1_pawn();
            if (auto at = p1 ? core::location(p1) : std::nullopt; at && std::hypot(at->y - row.y, at->z - row.z) > kReconcileUU) {
                reflect::Frame tp(s.actor_teleport);
                tp.arg(0, core::Vec3{row.x, row.y, row.z}).arg_bool(1, false).arg_bool(3, true);
                if (tp.invoke(p1)) ++g_reconciles;
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
        if (!guard::arm()) coop::leave("guard unavailable (fail closed)");
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
    case proto::Msg::Act: {
        proto::Act a;
        if (!proto::decode(msg, a) || a.dyn) return void(++g_act_bad);
        auto& last = g_act_last[a.net_id];
        if (a.seq <= last) return; // idempotent per enemy
        last = a.seq;
        const auto* name = g_names.text(a.net_id);
        const auto* cls = g_names.text(a.ability);
        const auto* cs = a.custom_spawn ? g_names.text(a.custom_spawn) : nullptr;
        const auto* ca = a.custom_attach ? g_names.text(a.custom_attach) : nullptr;
        if (!name || !cls) return void(++g_act_bad);
        auto it = g_index.find(*name);
        auto* enemy = it == g_index.end() ? nullptr : it->second.get();
        if (!enemy) return void(++g_act_absent); // not streamed in locally
        std::string why;
        if (!ability::replay(enemy, a, *cls, cs ? *cs : "", ca ? *ca : "", &why)) {
            if (++g_act_bad <= 5) log::warn("coop: act {} on {} rejected: {}", *cls, *name, why);
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
        if (proto::decode(msg, c)) log::warn("coop: host says: {}", c.text);
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
        g_taken.clear(); // the previous map's enemies are gone (unload ≠ death)
        g_world_done.clear();
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
            tp.arg(0, p).arg_bool(1, false).arg_bool(3, true);
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
    g_seat = 0, g_epoch = 0, g_latest = 0;
    g_ring.reset();
    g_names.clear();
    g_act_last.clear();
    g_world_done.clear();
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

std::string trace(const std::string& name) {
    std::string out;
    for (float y : g_trace) out += std::format("{:.1f} ", y);
    g_trace_name = name;
    g_trace.clear();
    return out.empty() ? "(tracing " + name + ")" : out;
}

std::string describe() {
    return std::format("client: seat=P{} epoch={} latest={} names={} taken={} applied={} badSnaps={} unknownNames={} animSets={} animFail={} copies={} reconciles={} kills={} actBad={} actAbsent={} rechecks={} aiLeaks={} world={} equipRequests={} motionWrites={} presses(attack)={}\n{}{}", g_seat,
                       g_epoch, g_latest, g_names.size(), g_taken.size(), g_applied, g_bad_snaps, g_unknown_names, g_anim_sets, g_anim_fail, g_copies.size(), g_reconciles, g_kills, g_act_bad, g_act_absent, g_rechecks, g_leaks, g_world_presented, g_equip_requests, g_motion_writes, g_input.presses[3], ability::describe(),
                       guard::describe());
}

void install() { coop::set_handlers(Role::Client, {on_connected, on_closed, on_reliable, on_unreliable, tick, stop}); }

} // namespace elmp::coop::client
