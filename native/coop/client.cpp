// Coop client (first slice): guest key → Build; Welcome arms the guard; every frame the local P1 input
// snapshot goes upstream (axes, held bits, cumulative press counters, snapshot ack); snapshots are decoded
// against the acknowledged base; enemies named by their spawn point are taken over (guard mark on pawn +
// controller, actor tick off, CanBePaused untouched by vanilla since activation is gated) and placed /
// HP-written from the host rows. The client process never settles (guard), never saves (guard).

#include "coop/client.h"

#include "coop/session.h"

#include "guard/guard.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/binary_checks.h"
#include "engine/lifecycle.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "input/pad_state.h"

#include "net/protocol.h"
#include "net/snapshot.h"

#include "base/clock.h"
#include "base/log.h"
#include "base/metrics.h"

#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace elmp::coop::client {

namespace {

namespace snap = net::snap;
namespace proto = net::proto;

constexpr std::size_t kRing = 32;

struct Taken {
    ObjHandle enemy, controller, spine;
    std::uint16_t anim = 0;
};

std::uint8_t g_seat = 0;
std::uint16_t g_epoch = 0, g_host_map = 0;
std::uint32_t g_latest = 0;
std::string g_follow_tag;
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
reflect::FuncRef g_set_tick;
bool g_resolved = false;
std::uint64_t g_unknown_names = 0, g_bad_snaps = 0, g_applied = 0;
metrics::Histogram& h_apply = metrics::histogram("coop.client.apply_us");

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

void take(const std::string& name, ue::UObject* enemy) {
    auto* controller = core::schema::script() ? core::schema::script()->pawn_controller.object(enemy) : nullptr;
    guard::mark(enemy);
    guard::mark(controller);
    if (g_set_tick.ok()) {
        reflect::Frame f(g_set_tick);
        f.arg_bool(0, false);
        f.invoke(enemy);
    }
    g_taken[name] = {ObjHandle::of(enemy), ObjHandle::of(controller)};
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
        if (auto at = core::location(enemy); at && (std::fabs(at->x - row.x) + std::fabs(at->y - row.y) + std::fabs(at->z - row.z)) > 1.0f) {
            reflect::Frame tp(s->actor_teleport);
            tp.arg(0, core::Vec3{row.x, row.y, row.z}).arg_bool(1, false).arg_bool(3, true);
            tp.invoke(enemy);
        }
        if (auto* hp = s->zc_hp.object(enemy)) *s->hp_curr.at<std::int32_t>(hp) = row.hp; // property write (no SetCurrHP in vanilla)
        auto& t = g_taken[*name];
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
                f.invoke(spine);
                t.anim = row.anim;
            }
        }
    }
    ++g_applied;
    h_apply.add(static_cast<double>(clock::now_us() - t0));
}

void on_connected(std::uint32_t) {
    g_ring = std::make_unique<std::array<snap::Frame, kRing>>();
    const auto key = guest_key();
    coop::send_reliable(0, std::span(g_buf.data(), proto::encode(proto::Msg::Build, proto::Json{std::format("{{\"guestKey\":\"{}\"}}", key)}, g_buf)));
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

void tick(std::int64_t now_us) {
    if (!g_seat || lifecycle::state() != lifecycle::State::Settled) return;
    send_input();
    follow(now_us);
}

void stop() {
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
    g_index.clear();
    g_index_version = ~0ull;
    g_input = {};
}

void on_closed(std::uint32_t, const std::string&, net::Reject) {}

} // namespace

std::string describe() {
    return std::format("client: seat=P{} epoch={} latest={} names={} taken={} applied={} badSnaps={} unknownNames={} presses(attack)={}\n{}", g_seat, g_epoch, g_latest,
                       g_names.size(), g_taken.size(), g_applied, g_bad_snaps, g_unknown_names, g_input.presses[3], guard::describe());
}

void install() { coop::set_handlers(Role::Client, {on_connected, on_closed, on_reliable, on_unreliable, tick, stop}); }

} // namespace elmp::coop::client
