#include "engine/world.h"

#include "engine/handle.h"
#include "engine/reflect.h"

#include "base/log.h"

namespace elmp::world {

using namespace RC::Unreal;

namespace {

struct Schema {
    bool tried = false, ok = false;
    std::string failure;
    reflect::PropRef engine_viewport, viewport_world, world_instance, instance_players, player_controller, controller_pawn;
    reflect::PropRef loader_map;
    reflect::FuncRef loader_is_loading, loader_is_ready, ksl_saved_dir;
    ObjHandle ksl;
};

Schema g;
ObjHandle g_engine, g_loader;
std::wstring g_saved;
metrics::Counter& g_scans = metrics::counter("reflect.scans");

// Resolved once; a failure is retried only when lifecycle asks (retry_schema), never per call.
bool resolve() {
    if (g.tried) return g.ok;
    UObject* eng = g_engine.get();
    if (!eng) return false;
    g.tried = true;
    std::string why;
    const auto take = [&](reflect::Resolver& r) {
        if (!r.ok() && why.empty()) why = r.failure();
    };
    reflect::Resolver e("Engine", eng->GetClassPrivate());
    g.engine_viewport = e.prop(L"GameViewport", reflect::Kind::Object);
    take(e);
    reflect::Resolver v("GameViewportClient", reflect::find_class(L"/Script/Engine.GameViewportClient"));
    g.viewport_world = v.prop(L"World", reflect::Kind::Object);
    take(v);
    reflect::Resolver w("World", reflect::find_class(L"/Script/Engine.World"));
    g.world_instance = w.prop(L"OwningGameInstance", reflect::Kind::Object);
    take(w);
    reflect::Resolver gi("GameInstance", reflect::find_class(L"/Script/Engine.GameInstance"));
    g.instance_players = gi.prop(L"LocalPlayers", reflect::Kind::Array);
    take(gi);
    reflect::Resolver p("Player", reflect::find_class(L"/Script/Engine.Player"));
    g.player_controller = p.prop(L"PlayerController", reflect::Kind::Object);
    take(p);
    reflect::Resolver c("Controller", reflect::find_class(L"/Script/Engine.Controller"));
    g.controller_pawn = c.prop(L"Pawn", reflect::Kind::Object);
    take(c);
    reflect::Resolver l("WorldLoaderSubsystem", reflect::find_class(L"/Script/Zenith.WorldLoaderSubsystem"));
    g.loader_map = l.prop(L"CurrentGameMapID", reflect::Kind::Name, sizeof(FName));
    g.loader_is_loading = l.func(L"IsLoading", 1);
    g.loader_is_ready = l.func(L"IsGameMapReady", 1);
    take(l);
    reflect::Resolver k("KismetSystemLibrary", reflect::find_class(L"/Script/Engine.KismetSystemLibrary"));
    g.ksl_saved_dir = k.func(L"GetProjectSavedDirectory", 0);
    take(k);
    g.ksl = ObjHandle::of(reflect::find_object(L"/Script/Engine.Default__KismetSystemLibrary"));
    if (why.empty() && g.ksl.empty()) why = "KismetSystemLibrary CDO missing";
    g.failure = why;
    g.ok = why.empty();
    if (!g.ok) log::error("world schema unresolved: {}", why);
    return g.ok;
}

struct RawArray {
    UObject** data;
    std::int32_t num, max;
};

} // namespace

void set_engine(UEngine* engine) {
    if (g_engine.empty() || g_engine.get() != reinterpret_cast<UObject*>(engine)) g_engine = ObjHandle::of(reinterpret_cast<UObject*>(engine));
}

UObject* engine() { return g_engine.get(); }

UObject* world() {
    if (!resolve()) return nullptr;
    UObject* vp = g.engine_viewport.object(engine());
    return vp ? g.viewport_world.object(vp) : nullptr;
}

UObject* game_instance() {
    UObject* w = world();
    return w ? g.world_instance.object(w) : nullptr;
}

UObject* p1_controller() {
    UObject* gi = game_instance();
    if (!gi) return nullptr;
    const auto& arr = *g.instance_players.at<RawArray>(gi);
    if (arr.num < 1 || !arr.data || !ue::alive(arr.data[0])) return nullptr;
    return g.player_controller.object(arr.data[0]);
}

UObject* p1_pawn() {
    UObject* pc = p1_controller();
    return pc ? g.controller_pawn.object(pc) : nullptr;
}

UObject* find_instance(UClass* cls) {
    if (!cls) return nullptr;
    ++g_scans;
    UObject* found = nullptr;
    UObjectGlobals::ForEachUObject([&](UObject* o, int32, int32) {
        if (o && o->GetClassPrivate() == cls && !o->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject | RF_ArchetypeObject)) && ue::alive(o)) {
            found = o;
            return RC::LoopAction::Break;
        }
        return RC::LoopAction::Continue;
    });
    return found;
}

UObject* world_loader() {
    if (UObject* l = g_loader.get()) return l;
    if (!resolve()) return nullptr;
    UObject* l = find_instance(reflect::find_class(L"/Script/Zenith.WorldLoaderSubsystem"));
    g_loader = ObjHandle::of(l);
    return l;
}

FName current_map_id() {
    UObject* l = g_loader.get();
    return l && g.ok ? g.loader_map.read<FName>(l) : FName();
}

bool loader_flags(bool& loading, bool& ready) {
    UObject* l = world_loader();
    if (!l) return false;
    reflect::Frame a(g.loader_is_loading);
    a.arg_bool(0, true);
    reflect::Frame b(g.loader_is_ready);
    b.arg_bool(0, true);
    if (!a.invoke(l) || !b.invoke(l)) return false;
    loading = a.ret_bool();
    ready = b.ret_bool();
    return true;
}

std::wstring saved_dir() {
    if (!g_saved.empty() || !resolve()) return g_saved;
    reflect::Frame f(g.ksl_saved_dir);
    if (f.invoke(g.ksl.get())) g_saved = f.ret_string();
    return g_saved;
}

void retry_schema() {
    if (!g.ok) g = Schema{};
}

std::string schema_status() {
    if (!g.tried) return "not resolved yet";
    return g.failure;
}

} // namespace elmp::world
