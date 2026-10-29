#include "coop/entities/spawn.h"

#include "core/player.h"
#include "core/schema.h"
#include "core/skin.h"

#include "engine/handle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/log.h"

#include <cstring>
#include <unordered_map>

namespace elmp::coop::entities {

namespace {

void write_xf(std::uint8_t* t, float x, float y, float z) { // FTransform: quat, translation(+pad), scale(+pad)
    std::memset(t, 0, 48);
    const float q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1}, p[3] = {x, y, z};
    std::memcpy(t, q, 16);
    std::memcpy(t + 16, p, 12);
    std::memcpy(t + 32, s, 12);
}

} // namespace

ue::UClass* class_by_path(const std::string& path) {
    static std::unordered_map<std::string, ObjHandle> cache;
    auto& h = cache[path];
    if (auto* c = h.get()) return static_cast<ue::UClass*>(c);
    auto* c = static_cast<ue::UClass*>(reflect::load_object(log::wide(path)));
    static ue::UClass* actor = reflect::find_class(L"/Script/Engine.Actor");
    if (!c || !actor || !c->IsChildOf(actor)) return nullptr;
    h = ObjHandle::of(c);
    return c;
}

ue::UObject* spawn_copy(ue::UClass* cls, float x, float y, float z) {
    static reflect::Resolver r("GameplayStatics", reflect::find_class(L"/Script/Engine.GameplayStatics"));
    static reflect::FuncRef begin = r.func(L"BeginDeferredActorSpawnFromClass", 5);
    static reflect::FuncRef finish = r.func(L"FinishSpawningActor", 2);
    auto* cdo = reflect::find_object(L"/Script/Engine.Default__GameplayStatics");
    if (!cls || !cdo || !begin.ok() || !finish.ok()) return nullptr;
    reflect::Frame b(begin);
    b.arg(0, world::game_instance()).arg(1, cls);
    if (auto* t = static_cast<std::uint8_t*>(b.raw(2))) write_xf(t, x, y, z);
    b.arg(3, static_cast<std::uint8_t>(1)); // ESpawnActorCollisionHandlingMethod::AlwaysSpawn
    b.arg(4, static_cast<ue::UObject*>(nullptr));
    if (!b.invoke(cdo)) return nullptr;
    auto* a = b.ret<ue::UObject*>();
    if (!ue::alive(a)) return nullptr;
    reflect::Frame fin(finish);
    fin.arg(0, a);
    if (auto* t = static_cast<std::uint8_t*>(fin.raw(1))) write_xf(t, x, y, z);
    fin.invoke(cdo);
    if (!ue::alive(a)) return nullptr;
    core::skin::normalize(a); // 复制出来的 Actor 会丢蓝图模板（渲染组件相对偏移 / 颜色变黑）
    return a;
}

void destroy_copy(ue::UObject* a) {
    static reflect::FuncRef destroy = reflect::Resolver("Actor", reflect::find_class(L"/Script/Engine.Actor")).func(L"K2_DestroyActor", 0);
    if (a && destroy.ok()) reflect::Frame(destroy).invoke(a);
}

bool move_copy(ue::UObject* a, float x, float y, float z) {
    const auto* s = core::schema::script();
    if (!a || !s) return false;
    reflect::Frame tp(s->actor_teleport);
    tp.arg(0, core::Vec3{x, y, z}).arg_bool(1, false).arg_bool(3, false);
    return tp.invoke(a);
}

} // namespace elmp::coop::entities
