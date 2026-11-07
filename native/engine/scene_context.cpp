#include "engine/scene_context.h"
#include "engine/reflect.h"
#include "engine/world.h"
#include <algorithm>
#include <format>
#include <unordered_set>
namespace elmp::scene_context {
namespace {
struct Schema {
 ObjHandle actor_class,point_class,scene_class,statics;
 reflect::FuncRef begin,finish,add,hide,collision,tick,remove,position,rotation,scale,scene_position;
 reflect::PropRef root,velocity;
 bool resolve(){
  if(actor_class.valid()&&point_class.valid()&&scene_class.valid()&&statics.valid())return true;
  auto* actor=reflect::find_class(L"/Script/Engine.Actor");reflect::Resolver a("scene context Actor",actor);
  root=a.prop(L"RootComponent",reflect::Kind::Object);add=a.func(L"AddComponentByClass",4);hide=a.func(L"SetActorHiddenInGame",1);collision=a.func(L"SetActorEnableCollision",1);tick=a.func(L"SetActorTickEnabled",1);remove=a.func(L"K2_DestroyActor",0);
  position=a.func(L"K2_SetActorLocation",4);rotation=a.func(L"K2_SetActorRotation",2);scale=a.func(L"SetActorScale3D",1);
  auto* scene=reflect::find_class(L"/Script/Engine.SceneComponent");reflect::Resolver c("scene context component",scene);velocity=c.prop(L"ComponentVelocity",reflect::Kind::Struct,12);scene_position=c.func(L"K2_SetWorldLocation",4);
  reflect::Resolver g("scene context spawn",reflect::find_class(L"/Script/Engine.GameplayStatics"));begin=g.func(L"BeginDeferredActorSpawnFromClass",5);finish=g.func(L"FinishSpawningActor",2);
  auto point=ObjHandle::of(reflect::find_class(L"/Script/Engine.TargetPoint"));auto gs=ObjHandle::of(reflect::find_object(L"/Script/Engine.Default__GameplayStatics"));
  if(!a.ok()||!c.ok()||!g.ok()||!point.valid()||!gs.valid())return false;
  actor_class=ObjHandle::of(actor);point_class=point;scene_class=ObjHandle::of(scene);statics=gs;return true;
 }
} s;
std::unordered_set<ObjHandle> live;
std::uint64_t created=0,destroyed=0,expired=0,failed=0;
struct Transform {float q[4]{0,0,0,1},p[3]{},pad0=0,scale[3]{1,1,1},pad1=0;};
static_assert(sizeof(Transform)==48);
}
void destroy(Context& value){if(auto* actor=value.actor.get()){if(s.resolve()&&reflect::Frame(s.remove).invoke(actor))++destroyed;else ++failed;}live.erase(value.actor);value={};}
bool create(Context& value,bool separate_aim){
 if(value.actor.valid()&&value.root.valid()&&(!separate_aim||value.aim.valid()))return true;
 destroy(value);if(!ue::on_game_thread()||!s.resolve()||!world::world())return ++failed,false;
 std::erase_if(live,[](const auto& h){if(h.valid())return false;++expired;return true;});if(live.size()>=1024)return ++failed,false;
 Transform x;reflect::Frame begin(s.begin);
 if(!begin.arg(0,world::world()).arg(1,s.point_class.get()).arg(2,x).arg(3,std::uint8_t{1}).arg(4,static_cast<ue::UObject*>(nullptr)).invoke(s.statics.get()))return ++failed,false;
 auto* actor=begin.ret<ue::UObject*>();if(!actor)return ++failed,false;value.actor=ObjHandle::of(actor);live.insert(value.actor);++created;
 reflect::Frame finish(s.finish);if(!finish.arg(0,actor).arg(1,x).invoke(s.statics.get())){destroy(value);return ++failed,false;}
 value.root=ObjHandle::of(s.root.object(actor));
 if(!value.root.valid()||!reflect::Frame(s.hide).arg_bool(0,true).invoke(actor)||!reflect::Frame(s.collision).arg_bool(0,false).invoke(actor)||!reflect::Frame(s.tick).arg_bool(0,false).invoke(actor)){destroy(value);return ++failed,false;}
 if(separate_aim){reflect::Frame add(s.add);if(!add.arg(0,s.scene_class.get()).arg_bool(1,true).arg(2,x).arg_bool(3,false).invoke(actor)){destroy(value);return ++failed,false;}value.aim=ObjHandle::of(add.ret<ue::UObject*>());if(!value.aim.valid()){destroy(value);return ++failed,false;}}
 else value.aim=value.root;
 return true;
}
bool pose(Context& value,const std::array<float,3>& at,const std::array<float,3>& rot,const std::array<float,3>& scale,const std::array<float,3>& velocity){
 if(!value.actor.valid()||!value.root.valid()||!s.resolve())return false;auto* actor=value.actor.get();
 if(!reflect::Frame(s.position).arg(0,at).arg_bool(1,false).arg_bool(3,false).invoke(actor)||!reflect::Frame(s.rotation).arg(0,rot).arg_bool(1,false).invoke(actor)||!reflect::Frame(s.scale).arg(0,scale).invoke(actor))return false;
 *s.velocity.at<std::array<float,3>>(value.root.get())=velocity;return true;
}
bool aim(Context& value,const std::array<float,3>& at,const std::array<float,3>& velocity){if(!value.aim.valid()||!s.resolve()||!reflect::Frame(s.scene_position).arg(0,at).arg_bool(1,false).arg_bool(3,false).invoke(value.aim.get()))return false;*s.velocity.at<std::array<float,3>>(value.aim.get())=velocity;return true;}
std::string describe(){std::erase_if(live,[](const auto& h){if(h.valid())return false;++expired;return true;});return std::format("scene contexts: live={} created={} destroyed={} expired={} failed={}\n",live.size(),created,destroyed,expired,failed);}
}
