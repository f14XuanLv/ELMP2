#include "coop/spine_root.h"
#include "engine/reflect.h"
#include "engine/binary_checks.h"
#include "base/hook.h"
#include "base/preflight.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>
namespace elmp::coop::spine_root {
namespace {
struct Schema {ObjHandle cls;reflect::FuncRef component;std::ptrdiff_t skeleton=0,begin=0,end=0,scale=0;} g;
struct Bound {ObjHandle actor,component;std::array<float,2> scale,original;};
std::unordered_map<std::uintptr_t,Bound> g_bound;
struct Component {ObjHandle component;};
std::unordered_map<ObjHandle,Component> g_components;
using Update=std::uintptr_t(__fastcall*)(void*);
Update g_update=nullptr;
std::uint64_t g_captured=0,g_applied=0,g_updates=0,g_expired=0,g_failed=0,g_restored=0;
bool read(void* out,std::uintptr_t address,std::size_t size){if(!address)return false;__try{std::memcpy(out,reinterpret_cast<const void*>(address),size);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool write(std::uintptr_t address,const std::array<float,2>& scale){__try{std::memcpy(reinterpret_cast<void*>(address),scale.data(),sizeof(scale));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
std::uintptr_t pointer(std::uintptr_t address){std::uintptr_t out=0;read(&out,address,8);return out;}
std::uintptr_t skeleton(ue::UObject* component){return component?pointer(reinterpret_cast<std::uintptr_t>(component)+g.skeleton):0;}
std::uintptr_t root(std::uintptr_t sk){
 const auto begin=pointer(sk+g.begin),end=pointer(sk+g.end);
 if(!begin||end<=begin||(end-begin)%8||(end-begin)/8>4096)return 0;
 return pointer(begin);
}
std::uintptr_t __fastcall update(void* sk){
 if(!g_bound.empty()&&ue::on_game_thread())if(auto it=g_bound.find(reinterpret_cast<std::uintptr_t>(sk));it!=g_bound.end()){
  auto& b=it->second;auto* c=b.component.get();
  if(!b.actor.valid()||!c||skeleton(c)!=reinterpret_cast<std::uintptr_t>(sk)){g_bound.erase(it);++g_expired;}
  else if(const auto r=root(reinterpret_cast<std::uintptr_t>(sk));r&&write(r+g.scale,b.scale))++g_updates;else ++g_failed;
 }
 return g_update(sk);
}
bool resolve(){
 if(g.cls.valid())return true;
 auto sk=binary_checks::offset("SpineSkeletonComponent.skeleton"),begin=binary_checks::offset("SpineSkeleton.bonesBegin"),end=binary_checks::offset("SpineSkeleton.bonesEnd"),scale=binary_checks::offset("SpineBone.scaleXY");
 if(!sk||!begin||!end||!scale)return false;
 auto* cls=reflect::find_class(L"/Script/SpinePlugin.SpineSkeletonComponent");reflect::Resolver a("root context actor",reflect::find_class(L"/Script/Engine.Actor"));g.component=a.func(L"GetComponentByClass",1);
 if(!cls||!a.ok())return false;g.cls=ObjHandle::of(cls);g.skeleton=*sk;g.begin=*begin;g.end=*end;g.scale=*scale;return true;
}
ue::UObject* component(ue::UObject* actor){
 if(g_components.size()>=512)std::erase_if(g_components,[](const auto& e){return !e.first.valid()||!e.second.component.valid();});
 if(g_components.size()>=512&&!g_components.contains(ObjHandle::of(actor)))return nullptr;
 const auto h=ObjHandle::of(actor);auto& cached=g_components[h];if(auto* c=cached.component.get())return c;
 reflect::Frame f(g.component);auto* c=f.arg(0,g.cls.get()).invoke(actor)?f.ret<ue::UObject*>():nullptr;cached.component=ObjHandle::of(c);return c;
}
bool valid(const std::array<float,2>& value){return std::all_of(value.begin(),value.end(),[](float f){return std::isfinite(f)&&std::abs(f)<=1000;});}
void restore(std::uintptr_t sk,const Bound& b){if(auto* c=b.component.get();c&&skeleton(c)==sk)if(auto r=root(sk);r&&write(r+g.scale,b.original))++g_restored;}
}
bool capture(ue::UObject* actor,std::array<float,2>& scale){
 if(!actor||!resolve())return ++g_failed,false;auto* c=component(actor);const auto sk=skeleton(c);if(!sk)return false;
 const auto r=root(sk);if(!r||!read(scale.data(),r+g.scale,sizeof(scale))||!valid(scale))return ++g_failed,false;++g_captured;return true;
}
bool apply(ue::UObject* actor,const std::array<float,2>& scale){
 if(!actor||!valid(scale)||!resolve())return ++g_failed,false;
 if(!g_update&&!hook::install_manifest("SpineSkeletonWorld",reinterpret_cast<void*>(&update),reinterpret_cast<void**>(&g_update)))return ++g_failed,false;
 auto* c=component(actor);const auto sk=skeleton(c),r=root(sk);if(!sk||!r)return ++g_failed,false;
 const auto owner=ObjHandle::of(actor);std::erase_if(g_bound,[&](const auto& e){if(e.second.actor==owner&&e.first!=sk){++g_expired;return true;}return !e.second.actor.valid()||!e.second.component.valid();});
 auto it=g_bound.find(sk);if(it==g_bound.end()||it->second.component!=ObjHandle::of(c)){
  if(g_bound.size()>=512)return ++g_failed,false;
  std::array<float,2> original;if(!read(original.data(),r+g.scale,sizeof(original)))return ++g_failed,false;
  it=g_bound.insert_or_assign(sk,Bound{owner,ObjHandle::of(c),scale,original}).first;
 }
 it->second.scale=scale;++g_applied;return true;
}
void forget(ue::UObject* actor){auto h=ObjHandle::of(actor);std::erase_if(g_bound,[&](const auto& e){if(e.second.actor!=h)return false;restore(e.first,e.second);return true;});g_components.erase(h);}
void reset(){for(const auto& e:g_bound)restore(e.first,e.second);g_bound.clear();g_components.clear();}
std::string describe(){return std::format("Spine root context: bound={} components={} captured={} applied={} updates={} expired={} restored={} failed={}\n",g_bound.size(),g_components.size(),g_captured,g_applied,g_updates,g_expired,g_restored,g_failed);}
}
