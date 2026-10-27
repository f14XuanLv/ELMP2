#include "coop/sync.h"

#include "guard/guard.h"

#include "core/player.h"
#include "core/schema.h"

#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/world.h"

#include "net/crypto.h"
#include "net/endpoint.h"

#include "base/clock.h"
#include "base/json.h"
#include "base/log.h"

#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>

namespace elmp::coop::sync {

namespace {

using RC::Unreal::UObject;

struct Refs {
    reflect::PropRef hidden, owner, psc_template, comp_active;
    reflect::FuncRef comp_owner, audio_playing;
    ue::UClass *zc = nullptr, *ability = nullptr, *psc = nullptr, *audio = nullptr, *actor = nullptr;
    bool tried = false;
};
Refs g;

const Refs& refs() {
    if (g.tried) return g;
    g.tried = true;
    reflect::Resolver a("Actor", reflect::find_class(L"/Script/Engine.Actor"));
    g.hidden = a.prop(L"bHidden", reflect::Kind::Bool);
    g.owner = a.prop(L"Owner", reflect::Kind::Object);
    reflect::Resolver c("ActorComponent", reflect::find_class(L"/Script/Engine.ActorComponent"));
    g.comp_active = c.prop(L"bIsActive", reflect::Kind::Bool);
    g.comp_owner = c.func(L"GetOwner", 0);
    g.zc = reflect::find_class(L"/Script/Zenith.ZenithCharacter");
    g.ability = reflect::find_class(L"/Script/Zenith.Ability");
    g.psc = reflect::find_class(L"/Script/Engine.ParticleSystemComponent");
    g.actor = reflect::find_class(L"/Script/Engine.Actor");
    if (g.psc) g.psc_template = reflect::Resolver("ParticleSystemComponent", g.psc).prop(L"Template", reflect::Kind::Object);
    // FMODStudio is a plugin module; absent → audio counts stay 0
    if ((g.audio = reflect::find_class(L"/Script/FMODStudio.FMODAudioComponent")))
        g.audio_playing = reflect::Resolver("FMODAudioComponent", g.audio).func(L"IsPlaying", 0);
    if (!a.ok() || !c.ok()) log::warn("sync: {} {}", a.failure(), c.failure());
    return g;
}

double round2(double v) { return std::round(v * 100.0) / 100.0; }

ue::UObject* spine_of(ue::UObject* actor, bool lily, const core::schema::Script& s) {
    if (lily) {
        const auto* bp = core::schema::blueprint();
        return bp ? bp->lily_spine.object(actor) : nullptr;
    }
    static ObjHandle cls = ObjHandle::of(reflect::find_object(L"/Script/SpineExtensionPlugin.SpineSkeletonAnimationExComponent"));
    reflect::Frame f(s.actor_component_by_class);
    f.arg(0, cls.get());
    return f.invoke(actor) ? f.ret<ue::UObject*>() : nullptr;
}

} // namespace

bool wants(const std::string& cat, const char* c) { return cat.empty() || cat == "all" || cat == c; }

void header(std::string& out, const char* role, int seat, const std::string& extra) {
    out += std::format("{{\"cat\":\"hdr\",\"role\":\"{}\",\"seat\":{},\"map\":\"{}\",\"t_us\":{}{}{}}}\n", role, seat, ue::utf8(world::current_map_id()), clock::now_us(),
                       extra.empty() ? "" : ",", extra);
}

void entity(std::string& out, const Ent& e) {
    const auto* s = core::schema::script();
    const auto& r = refs();
    if (!s || !ue::alive(e.actor)) return;
    json::Value v = json::Value::object();
    v.set("cat", "ent");
    v.set("k", e.key);
    v.set("cls", log::utf8(ue::class_name_of(e.actor)));
    if (auto at = core::location(e.actor)) {
        v.set("x", round2(at->x));
        v.set("y", round2(at->y - e.yoff));
        v.set("z", round2(at->z));
    }
    v.set("slot", e.slot);
    if (r.zc && e.actor->GetClassPrivate()->IsChildOf(r.zc))
        if (auto* hp = s->zc_hp.object(e.actor)) {
            v.set("hp", static_cast<std::int64_t>(s->hp_curr.read<std::int32_t>(hp)));
            v.set("mhp", static_cast<std::int64_t>(s->hp_max.read<std::int32_t>(hp)));
        }
    if (r.hidden.ok()) v.set("vis", !r.hidden.read_bool(e.actor));
    if (auto* spine = spine_of(e.actor, e.lily, *s)) {
        reflect::Frame cur(s->sp_get_current);
        cur.arg(0, 0);
        if (cur.invoke(spine))
            if (auto* entry = cur.ret<ue::UObject*>(); ue::alive(entry)) {
                reflect::Frame nm(s->te_animation_name);
                if (nm.invoke(entry)) v.set("anim", log::utf8(nm.ret_string()));
                reflect::Frame t(s->te_track_time);
                if (t.invoke(entry)) v.set("t", round2(t.ret<float>()));
            }
    }
    out += json::dump(v, 0) + "\n";
}

void fx(std::string& out, const KeyOf& key_of) {
    const auto& r = refs();
    struct Count {
        int abilities = 0, psc = 0, audio = 0;
    };
    std::map<std::string, Count> by;
    std::map<ue::UObject*, std::string> keys; // owner → key (memo for this walk)
    auto key = [&](ue::UObject* owner) -> std::string {
        if (!owner) return "-";
        auto [it, fresh] = keys.try_emplace(owner);
        if (fresh) {
            it->second = key_of(owner);
            if (it->second.empty()) it->second = "?" + log::utf8(ue::class_name_of(owner));
        }
        return it->second;
    };
    auto owner_of = [&](ue::UObject* comp) -> ue::UObject* {
        if (!r.comp_owner.ok()) return nullptr;
        reflect::Frame f(r.comp_owner);
        return f.invoke(comp) ? f.ret<ue::UObject*>() : nullptr;
    };
    const auto flags = static_cast<RC::Unreal::EObjectFlags>(RC::Unreal::RF_ClassDefaultObject | RC::Unreal::RF_ArchetypeObject);
    RC::Unreal::UObjectGlobals::ForEachUObject([&](UObject* o, std::int32_t, std::int32_t) {
        if (!o || o->HasAnyFlags(flags) || !ue::alive(o)) return RC::LoopAction::Continue;
        auto* cls = o->GetClassPrivate();
        if (r.ability && cls->IsChildOf(r.ability)) {
            ++by[key(r.owner.ok() ? r.owner.object(o) : nullptr)].abilities;
        } else if (r.psc && cls->IsChildOf(r.psc)) {
            if (r.comp_active.ok() && r.comp_active.read_bool(o) && r.psc_template.object(o)) ++by[key(owner_of(o))].psc;
        } else if (r.audio && cls->IsChildOf(r.audio) && r.audio_playing.ok()) {
            reflect::Frame f(r.audio_playing);
            if (f.invoke(o) && f.ret_bool()) ++by[key(owner_of(o))].audio;
        }
        return RC::LoopAction::Continue;
    });
    for (const auto& [k, c] : by) {
        json::Value v = json::Value::object();
        v.set("cat", "fx");
        v.set("owner", k);
        v.set("abilities", c.abilities);
        v.set("psc", c.psc);
        v.set("audio", c.audio);
        out += json::dump(v, 0) + "\n";
    }
}

void world_objects(std::string& out) {
    const auto& r = refs();
    const auto* s = core::schema::script();
    static reflect::PropRef breakable_hp =
        reflect::Resolver("Breakable", reflect::find_class(L"/Script/Zenith.Breakable")).prop(L"HPComponent", reflect::Kind::Object);
    using K = registry::Kind;
    for (auto kind : {K::Interactable, K::Breakable, K::Trigger, K::RestingPoint}) {
        for (const auto& e : registry::bucket(kind)) {
            auto* a = e.handle.get();
            if (!a) continue;
            json::Value v = json::Value::object();
            v.set("cat", "world");
            v.set("k", std::format("w:{}", ue::utf8_name(a)));
            v.set("kind", registry::kind_name(kind));
            if (r.hidden.ok()) v.set("vis", !r.hidden.read_bool(a));
            if (kind == K::Breakable && s && breakable_hp.ok())
                if (auto* hp = breakable_hp.object(a)) v.set("hp", static_cast<std::int64_t>(s->hp_curr.read<std::int32_t>(hp)));
            out += json::dump(v, 0) + "\n";
        }
    }
    std::map<std::string, int> picks; // collectables: dynamic names differ per process → count per class
    for (const auto& e : registry::bucket(K::Collectable))
        if (auto* a = e.handle.get(); a && (!r.hidden.ok() || !r.hidden.read_bool(a))) ++picks[log::utf8(ue::class_name_of(a))];
    for (const auto& [cls, n] : picks) out += std::format("{{\"cat\":\"world\",\"k\":\"pk:{}\",\"kind\":\"Collectable\",\"n\":{}}}\n", cls, n);
}

void guard_counters(std::string& out) {
    const auto c = guard::counters();
    out += std::format("{{\"cat\":\"cnt\",\"src\":\"guard\",\"armed\":{},\"damage_blocked\":{},\"damage_replayed\":{},\"skill_blocked\":{},\"skill_replayed\":{},"
                       "\"save_blocked\":{},\"ai_activate_blocked\":{},\"char_activate_blocked\":{},\"collision_blocked\":{},\"tick_blocked\":{},\"isplayer_false\":{}}}\n",
                       guard::armed(), c.damage_blocked, c.damage_replayed, c.skill_blocked, c.skill_replayed, c.save_blocked, c.ai_activate_blocked, c.char_activate_blocked,
                       c.collision_blocked, c.tick_blocked, c.isplayer_false);
}

namespace {
struct RecEnt {
    ObjHandle actor, spine;
    bool lily = false;
    std::string last;
};
struct Rec {
    EntList list;
    int frames = 0, left = 0, n = 0;
    std::map<std::string, RecEnt> ents;
    std::vector<std::string> lines;
    std::size_t dropped = 0;
};
Rec g_rec;

void rec_refresh() {
    const auto* s = core::schema::script();
    if (!s || !g_rec.list) return;
    for (const auto& e : g_rec.list()) {
        auto& r = g_rec.ents[e.key];
        if (r.actor.get() != e.actor) r.actor = ObjHandle::of(e.actor), r.spine = ObjHandle::of(spine_of(e.actor, e.lily, *s)), r.lily = e.lily;
    }
}
} // namespace

std::string rec_start(EntList list, int frames) {
    g_rec = Rec{};
    g_rec.list = std::move(list);
    g_rec.frames = g_rec.left = std::clamp(frames, 1, 36000);
    rec_refresh();
    return std::format("recording {} frames, {} entities\n", g_rec.frames, g_rec.ents.size());
}

void rec_tick() {
    if (g_rec.left <= 0) return;
    const auto* s = core::schema::script();
    const auto& r = refs();
    if (!s) return;
    --g_rec.left;
    ++g_rec.n;
    if (g_rec.n % 30 == 0) rec_refresh();
    for (auto& [k, e] : g_rec.ents) {
        auto* a = e.actor.get();
        std::string anim = "-";
        if (auto* spine = e.spine.get()) {
            reflect::Frame cur(s->sp_get_current);
            cur.arg(0, 0);
            if (cur.invoke(spine))
                if (auto* entry = cur.ret<ue::UObject*>(); ue::alive(entry)) {
                    reflect::Frame nm(s->te_animation_name);
                    if (nm.invoke(entry)) anim = log::utf8(nm.ret_string());
                }
        }
        std::int32_t hp = -1;
        if (a && r.zc && a->GetClassPrivate()->IsChildOf(r.zc))
            if (auto* h = s->zc_hp.object(a)) hp = s->hp_curr.read<std::int32_t>(h);
        const bool vis = a && r.hidden.ok() && !r.hidden.read_bool(a);
        auto state = a ? std::format("{}|{}|{}", anim, hp, vis ? 1 : 0) : std::string("gone");
        if (state == e.last) continue;
        e.last = state;
        if (g_rec.lines.size() >= 20000) {
            ++g_rec.dropped;
            continue;
        }
        json::Value v = json::Value::object();
        v.set("cat", "rec");
        v.set("f", g_rec.n);
        v.set("k", k);
        if (a) {
            v.set("anim", anim);
            v.set("hp", static_cast<std::int64_t>(hp));
            v.set("vis", vis);
        } else {
            v.set("gone", true);
        }
        g_rec.lines.push_back(json::dump(v, 0));
    }
}

std::string rec_dump() {
    std::string out = std::format("{{\"cat\":\"rech\",\"frames\":{},\"done\":{},\"recorded\":{},\"entities\":{},\"dropped\":{}}}\n", g_rec.frames, g_rec.left <= 0, g_rec.n,
                                  g_rec.ents.size(), g_rec.dropped);
    for (const auto& l : g_rec.lines) out += l + "\n";
    return out;
}

void hashes(std::string& out) {
    namespace fs = std::filesystem;
    const fs::path saved(world::saved_dir());
    for (const auto* sub : {L"SaveGames", L"ELMP\\ELMP"}) {
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(saved / sub, ec)) {
            if (!f.is_regular_file()) continue;
            std::ifstream in(f.path(), std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
            const auto d = net::crypto::sha256(bytes);
            json::Value v = json::Value::object();
            v.set("cat", "hash");
            v.set("file", log::utf8((fs::path(sub) / f.path().filename()).wstring()));
            v.set("sha", net::to_hex(std::span(d.data(), 8)));
            v.set("bytes", static_cast<std::int64_t>(bytes.size()));
            out += json::dump(v, 0) + "\n";
        }
    }
}

} // namespace elmp::coop::sync
