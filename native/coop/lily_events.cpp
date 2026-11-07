#include "coop/lily_events.h"
#include "engine/reflect.h"
#include "core/schema.h"
#include "base/clock.h"
#include "base/log.h"
#include <Unreal/FString.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>
namespace elmp::coop::lily_events {
namespace {
struct Schema {
 ObjHandle cls;
 reflect::PropRef layers,definitions,track,name,loop,hold,start,end,blend,root;
 reflect::FuncRef component,play,stop,track_time,track_name,set_time;
 reflect::PropRef actor_scale;
 reflect::FuncRef global_time,get_scale,set_scale;
 ObjHandle gameplay;
 int stride=0;
 bool resolve(){
  if(cls.valid())return true;
  auto* type=reflect::find_class(L"/Script/Zenith.SpineAnimationComponent");reflect::Resolver c("Lily external layers",type);
  layers=c.prop(L"AnimationDataLayers",reflect::Kind::Array);play=c.func(L"PlayAnimationDefinition",2);stop=c.func(L"StopAnimation",3);
  reflect::Resolver a("Lily event actor",reflect::find_class(L"/Script/Engine.Actor"));component=a.func(L"GetComponentByClass",1);
  actor_scale=a.prop(L"CustomTimeDilation",reflect::Kind::Float,4);
  reflect::Resolver world("Lily event world clock",reflect::find_class(L"/Script/Engine.GameplayStatics"));global_time=world.func(L"GetGlobalTimeDilation",1);gameplay=ObjHandle::of(reflect::find_object(L"/Script/Engine.Default__GameplayStatics"));
  reflect::Resolver d("Lily runtime layer",reflect::find_class(L"/Script/Zenith.SpineAnimationRuntimeData"));definitions=d.prop(L"AnimDefinitions",reflect::Kind::Array);track=d.prop(L"TrackEntry",reflect::Kind::Object);stride=reflect::element_size(definitions);
  reflect::Resolver f("Lily animation definition",static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.SpineAnimationDefinition")));
  name=f.prop(L"AnimationName",reflect::Kind::Str);loop=f.prop(L"bLoop",reflect::Kind::Bool);hold=f.prop(L"bHold",reflect::Kind::Bool);root=f.prop(L"bUseRootMotion",reflect::Kind::Bool);start=f.prop(L"StartTime",reflect::Kind::Float,4);end=f.prop(L"EndTime",reflect::Kind::Float,4);blend=f.prop(L"BlendIn",reflect::Kind::Float,4);
  const auto* s=core::schema::script();if(!s||!c.ok()||!a.ok()||!d.ok()||!f.ok()||stride<=0||reflect::element_size(layers)!=8)return false;
  track_time=s->te_track_time;track_name=s->te_animation_name;reflect::Resolver t("Lily event track",reflect::find_class(L"/Script/SpinePlugin.TrackEntry"));set_time=t.func(L"SetTrackTime",1);get_scale=t.func(L"GetTimeScale",0);set_scale=t.func(L"SetTimeScale",1);if(!t.ok()||!world.ok()||!gameplay.valid())return false;cls=ObjHandle::of(type);return true;
 }
 ue::UObject* of(ue::UObject* pawn){reflect::Frame f(component);return f.arg(0,cls.get()).invoke(pawn)?f.ret<ue::UObject*>():nullptr;}
 ue::UObject* data(ue::UObject* c,unsigned i){const auto& a=reflect::array_at(c,layers);return a.num>static_cast<int>(i)&&a.num<=16&&a.data?reinterpret_cast<ue::UObject**>(a.data)[i]:nullptr;}
} g;
struct Layer { std::uint32_t generation=0,sample=0;net::snap::EventAnimation last;std::int64_t seen=0;float rate=1; };
struct Entry { ObjHandle component;std::array<Layer,10> layers; };
std::unordered_map<ObjHandle,Entry> g_entries;
std::uint64_t g_captured=0,g_played=0,g_stopped=0,g_failed=0,g_phase=0;
}
bool capture_layer(ue::UObject* pawn,unsigned layer,Name intern,net::snap::EventAnimation& out){
 out={};if(layer>9||!pawn||!intern||!g.resolve())return ++g_failed,false;auto* component=g.of(pawn);if(!component)return ++g_failed,false;
 {auto* data=g.data(component,layer);if(!ue::alive(data))return true;auto* track=g.track.object(data);if(!track)return true;
  reflect::Frame n(g.track_name),t(g.track_time);if(!n.invoke(track)||!t.invoke(track))return ++g_failed,false;const auto name=n.ret_string();if(name.empty())return true;
  const auto& a=reflect::array_at(data,g.definitions);if(a.num<0||a.num>32||(a.num&&!a.data))return ++g_failed,false;
  for(int j=0;j<a.num;++j){auto* d=a.data+j*g.stride;const auto& chars=g.name.at<RC::Unreal::FString>(d)->GetCharArray();if(chars.Num()<1||std::wstring(chars.GetData(),chars.Num()-1)!=name)continue;
   auto& e=out;e.generation=static_cast<std::uint32_t>(ObjHandle::of(track).serial);e.animation=intern(log::utf8(name));e.flags=(g.loop.read_bool(d)?1:0)|(g.hold.read_bool(d)?2:0);e.time=std::max(0.0f,t.ret<float>());e.start=g.start.read<float>(d);e.end=g.end.read<float>(d);e.blend=g.blend.read<float>(d);break;
  }
 }
 ++g_captured;return true;
}
bool apply_layer(ue::UObject* pawn,unsigned layer,const net::snap::EventAnimation& value,std::uint32_t sample,Text text){
 std::erase_if(g_entries,[](const auto& v){return !v.first.valid();});
 if(g_entries.size()>=256&&!g_entries.contains(ObjHandle::of(pawn)))return ++g_failed,false;
 if(layer>9||!pawn||!text||!g.resolve())return ++g_failed,false;auto& record=g_entries[ObjHandle::of(pawn)];auto* component=record.component.get();if(!component){component=g.of(pawn);record={};record.component=ObjHandle::of(component);}if(!component)return ++g_failed,false;
 const auto now=clock::now_us();float actor_clock=0;
 if(value.generation!=0){
  reflect::Frame world(g.global_time);if(!world.arg(0,pawn).invoke(g.gameplay.get()))return ++g_failed,false;
  actor_clock=world.ret<float>()*g.actor_scale.read<float>(pawn);
 }
 {const auto& e=value;auto& old=record.layers[layer];auto* data=g.data(component,layer);auto* track=ue::alive(data)?g.track.object(data):nullptr;
  if(!e.generation){if(old.generation){if(!reflect::Frame(g.stop).arg(0,static_cast<std::uint8_t>(layer)).arg_bool(1,false).arg(2,0.0f).invoke(component))return ++g_failed,false;++g_stopped;}old={};return true;}
  if(old.generation!=e.generation||!track){
   const auto* name=text(e.animation);if(!name)return ++g_failed,false;reflect::Frame call(g.play);auto* definition=call.raw(0);
   auto* string=g.name.at<RC::Unreal::FString>(definition);std::destroy_at(string);std::construct_at(string,log::wide(*name).c_str());
   g.loop.write_bool(definition,e.flags&1);g.hold.write_bool(definition,e.flags&2);g.root.write_bool(definition,false);
   *g.start.at<float>(definition)=e.start;*g.end.at<float>(definition)=e.end;*g.blend.at<float>(definition)=e.blend;
   // New parameter storage contains no Host completion delegates or movement side effects.
   if(!call.arg(1,static_cast<std::uint8_t>(layer)).invoke(component))return ++g_failed,false;
   data=g.data(component,layer);track=ue::alive(data)?g.track.object(data):nullptr;old={};old.generation=e.generation;++g_played;
  }
  if(old.sample!=sample){
   if(old.sample&&sample>old.sample)old.rate=std::clamp((e.time-old.last.time)/(static_cast<float>(sample-old.sample)/1000),0.0f,10.0f);
   old.sample=sample;old.last=e;old.seen=now;
  }
  if(track){
   // Own-seat prediction can keep a full-speed World while Host death slows down. Use the original
   // track clock too; phase correction alone would repeatedly rewind a full-speed death animation.
   if(actor_clock>0.000001f){const float rate=std::clamp(old.rate/actor_clock,0.0f,1000.0f);reflect::Frame current(g.get_scale);if(!current.invoke(track))return ++g_failed,false;if(std::abs(current.ret<float>()-rate)>.005f&&!reflect::Frame(g.set_scale).arg(0,rate).invoke(track))return ++g_failed,false;}
   const float time=e.time+old.rate*std::min(static_cast<float>(now-old.seen)/1'000'000,0.1f);reflect::Frame current(g.track_time);if(current.invoke(track)&&std::abs(current.ret<float>()-time)>.03f){if(!reflect::Frame(g.set_time).arg(0,time).invoke(track))return ++g_failed,false;++g_phase;}
  }
 }
 std::erase_if(g_entries,[](const auto& v){return !v.first.valid();});return true;
}
bool capture(ue::UObject* pawn,Name name,net::snap::LilyEvents& out){out={};for(unsigned i=0;i<4;++i)if(!capture_layer(pawn,i+6,name,out.layers[i]))return false;return true;}
bool apply(ue::UObject* pawn,const net::snap::LilyEvents& v,std::uint32_t sample,Text text){for(unsigned i=0;i<4;++i)if(!apply_layer(pawn,i+6,v.layers[i],sample,text))return false;return true;}
void reset(){
 for(const auto& [_,entry]:g_entries)if(auto* component=entry.component.get())
  for(unsigned i=0;i<10;++i)if(entry.layers[i].generation){if(reflect::Frame(g.stop).arg(0,static_cast<std::uint8_t>(i)).arg_bool(1,false).arg(2,0.0f).invoke(component))++g_stopped;else ++g_failed;}
 g_entries.clear();
}
std::string describe(){return std::format("Lily external layers: cached={} captured={} played={} stopped={} phase={} failed={}\n",g_entries.size(),g_captured,g_played,g_stopped,g_phase,g_failed);}
}
