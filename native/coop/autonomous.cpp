#include "coop/autonomous.h"
#include "coop/command_context.h"
#include "coop/lily_events.h"
#include "coop/session.h"
#include "core/schema.h"
#include "core/player.h"
#include "engine/reflect.h"
#include "engine/registry.h"
#include "engine/binary_checks.h"
#include "engine/scene_context.h"
#include "engine/world.h"
#include "input/pad_state.h"
#include "base/hook.h"
#include "base/preflight.h"
#include "base/clock.h"
#include "base/log.h"
#include "base/metrics.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <unordered_map>

namespace elmp::coop::autonomous {
namespace {
namespace p=net::proto;
using O=ue::UObject;using V=std::array<float,3>;
struct Schema {
 ObjHandle spirit,ai,follow_class,anchor_class,summoner_class,summon_class;
 reflect::PropRef ai_class,queue_pawn,command_pawn,snapshot,brain,scale,velocity,spirit_queue,spirit_locomotion,spirit_class;
 reflect::FuncRef summoner,active,get_spirit,get_command,add,clear,request_end,stop_logic,running,disappear,face,loc,rotation,set_loc,set_rotation,world_delta,direction,button;
 ObjHandle statics;
 reflect::FuncRef component,follow_get,follow_set,follow_clear,follow_enabled,scene_position,scene_rotation,scene_scale,set_scale,hidden,collision,tick;
 reflect::PropRef scene_velocity;
 std::array<int,4> history{};
 int source_offset=0,cached_spirit_offset=0;
 bool ready()const{return spirit.valid()&&ai.valid();}
} g;
struct Role {
 struct FollowStep {p::SpiritFrame frame;float dt=0;bool begin=false;};
 ObjHandle actor,caster,source,queue,command,follow,anchor,original_target;
 scene_context::Context anchor_context;
 std::uint16_t seat=0,class_id=0,summon_id=0;
 std::uint32_t id=0,episode=0,generation=0,sequence=0,frame=0,summon_input=0;
 std::vector<p::SpiritEdge> edges;
 std::int64_t seen=0,last=0;
 float original=1;
 bool bound=false,started=false,updating=false;
 std::deque<p::SpiritFrame> frames;
 std::deque<FollowStep> follow_steps;
 float elapsed=0,rate=1,world_rate=1;
 unsigned borrowed=0;
 std::string waiting_for;
};
std::map<std::uint32_t,Role> roles;
std::unordered_map<ObjHandle,std::uint32_t> actors;
struct Waiting { float scale=1;std::int64_t since=0; };
std::unordered_map<ObjHandle,Waiting> waiting;
std::unordered_map<std::uint16_t,ObjHandle> classes;
FrameSink sink=nullptr;native_lily::Name name=nullptr;native_lily::Text text=nullptr;
std::uint16_t own_seat=0,current_map=0;
std::uint32_t next_role=0,next_sequence=0;
std::uint64_t captured=0,received=0,played=0,starts=0,ends=0,bindings=0,paused_brains=0,removed=0,starved=0,anchors_created=0,anchors_removed=0,follow_played=0,unexpected_starts=0;
std::string failure;
using Update=void(__fastcall*)(O*,float);Update original_update=nullptr;
using FollowTick=void(__fastcall*)(O*,float,std::int32_t,void*);FollowTick original_follow=nullptr;
using SpiritStart=std::uintptr_t(__fastcall*)(O*,O*);
bool replaying=false;
auto& buffer=metrics::histogram("autonomous.buffer_ms");
auto& corrections=metrics::histogram("autonomous.correction_uu");
void fail(std::string s){if(failure.empty())failure=std::move(s);}
bool is_active(O* actor){reflect::Frame q(g.active);return actor&&q.invoke(actor)&&q.ret<bool>();}
O* get_summoner(O* a){reflect::Frame q(g.summoner);return a&&a->IsA(static_cast<ue::UClass*>(g.spirit.get()))&&q.invoke(a)?q.ret<O*>():nullptr;}
Role* role_of(O* a){const auto i=actors.find(ObjHandle::of(a));if(i==actors.end())return nullptr;const auto r=roles.find(i->second);return r==roles.end()?nullptr:&r->second;}
Role* owner_role(O* a){if(auto* r=role_of(a))return r;if(a&&g.ready()&&a->IsA(static_cast<ue::UClass*>(g.ai.get())))if(const auto* s=core::schema::script())return role_of(s->controller_pawn.object(a));return nullptr;}
O* caster(std::uint16_t seat){return sink?(seat==1?world::p1_pawn():core::pawn(seat)):(seat==own_seat?world::p1_pawn():native_lily::actor(seat));}
std::uint16_t seat_of(O* a){if(a==world::p1_pawn())return sink?1:own_seat;const auto slot=core::slot_of(a);return slot>1&&slot<=8?static_cast<std::uint16_t>(slot):0;}
O* load(std::uint16_t id){if(classes.size()>=512&&!classes.contains(id)){fail("autonomous class cache capacity");return nullptr;}auto& weak=classes[id];if(auto* cls=weak.get())return cls;const auto* s=text?text(id):nullptr;auto* cls=s?reflect::load_object(log::wide(*s)):nullptr;weak=ObjHandle::of(cls);return cls;}
float world_dt(O* a){reflect::Frame f(g.world_delta);return f.arg(0,a).invoke(g.statics.get())?f.ret<float>():0;}
void pause(O* a){const auto* s=core::schema::script();auto* c=s?s->pawn_controller.object(a):nullptr;auto* brain=c?g.brain.object(c):nullptr;if(!brain)return;reflect::Frame running(g.running);if(running.invoke(brain)&&running.ret<bool>()){if(!reflect::Frame(g.stop_logic).arg_string(0,L"ELMP Host autonomous decisions").invoke(brain))fail("Spirit brain could not stop");else ++paused_brains;}}
bool eligible_impl(O* a){if(!a||!g.ready()||!a->IsA(static_cast<ue::UClass*>(g.spirit.get())))return false;auto* cls=g.ai_class.object(a);return cls&&static_cast<ue::UClass*>(cls)->IsChildOf(static_cast<ue::UClass*>(g.ai.get()));}
void release(Role& r){
 r.follow_steps.clear();
 if(auto* anchor=r.anchor.get()){
  const auto* s=core::schema::script();auto* component=r.follow.get();reflect::Frame target(g.follow_get);
  if(s&&component&&target.invoke(component)&&target.ret<O*>()==s->actor_root.object(anchor))reflect::Frame(g.follow_set).arg(0,r.original_target.get()).arg_bool(1,false).invoke(component);
  scene_context::destroy(r.anchor_context);++anchors_removed;
 }r.anchor={};r.follow={};r.original_target={};
 if(auto* a=r.actor.get()){*g.scale.at<float>(a)=r.original;if(r.borrowed&&text)lily_events::apply_layer(a,r.borrowed,{},0,text);}
 actors.erase(r.actor);r.actor={};r.queue={};r.bound=false;
}
Role* register_host(O* actor,O* command,bool fresh){
 if(!eligible_impl(actor)||!command)return nullptr;if(!command->IsA(static_cast<ue::UClass*>(g.summon_class.get()))){fail("autonomous source is not a native summon command");return nullptr;}auto* pawn=g.command_pawn.object(command);const auto seat=seat_of(pawn);if(!seat)return nullptr;
 auto* r=role_of(actor);
 if(!r){if(roles.size()>=128||next_role==UINT32_MAX){fail("autonomous role capacity/identity exhausted");return nullptr;}Role v;v.id=++next_role;v.actor=ObjHandle::of(actor);v.original=g.scale.read<float>(actor);v.episode=1;roles[v.id]=v;actors[v.actor]=v.id;r=&roles[v.id];}
 else if(fresh){++r->episode;r->sequence=0;r->command={};r->edges.clear();}
 r->seat=seat;r->caster=ObjHandle::of(pawn);r->generation=static_cast<std::uint32_t>(r->caster.serial);r->source=ObjHandle::of(command);r->queue=ObjHandle::of(g.spirit_queue.object(actor));r->seen=clock::now_us();
 r->class_id=name(log::utf8(actor->GetClassPrivate()->GetPathName()));r->summon_id=name(log::utf8(command->GetClassPrivate()->GetPathName()));
 if(fresh){native_lily::Source origin;if(native_lily::source(pawn,origin))r->summon_input=origin.seq;}return r;
}
void capture(Role& r,float dt,bool active,bool decision=false){
 auto* a=r.actor.get();auto* command=r.source.get();const auto* s=core::schema::script();if(!a||!command||!s||!sink||!name)return;
 const auto now=clock::now_us();p::SpiritFrame f;auto& c=f.context;c.seat=r.seat;c.generation=r.generation;c.seq=++r.frame;c.t_ms=static_cast<std::uint32_t>(now/1000);
 c.dt=std::clamp(dt,0.0f,60.0f);c.step=decision?.000001f:(r.last?std::clamp(static_cast<float>(now-r.last)/1e6f,0.001f,.05f):1/60.f);if(!decision)r.last=now;c.world_dt=std::clamp(world_dt(a),.000001f,60.f);f.decision_only=decision;
 if(auto at=core::location(a))c.position={at->x,at->y,at->z};else return;
 auto* move=s->char_movement.object(a);if(move){c.velocity=g.velocity.read<V>(move);c.mode=s->cm_mode.read<std::uint8_t>(move);}
 auto* loco=g.spirit_locomotion.object(a);reflect::Frame facing(s->loco_facing_right);c.facing=loco&&facing.invoke(loco)&&facing.ret<bool>()?2:1;
 reflect::Frame component(g.component);auto* follow=component.arg(0,g.follow_class.get()).invoke(a)?component.ret<O*>():nullptr;
 reflect::Frame target(g.follow_get);auto* scene=follow&&target.invoke(follow)?target.ret<O*>():nullptr;
 reflect::Frame enabled(g.follow_enabled);
 if(scene&&enabled.invoke(follow)&&enabled.ret<bool>()){reflect::Frame pos(g.scene_position),rot(g.scene_rotation),scale(g.scene_scale);if(pos.invoke(scene)&&rot.invoke(scene)&&scale.invoke(scene)){f.has_follow=true;f.follow_position=pos.ret<V>();f.follow_rotation=rot.ret<V>();f.follow_scale=scale.ret<V>();f.follow_velocity=g.scene_velocity.read<V>(scene);for(unsigned i=0;i<4;++i)std::memcpy(f.follow_history.data()+3*i,reinterpret_cast<std::uint8_t*>(follow)+g.history[i],12);}}
 if(auto* queue=r.queue.get()){auto in=g.snapshot.read<input::InputSnapshot>(queue);c.input={in.input_x,in.input_y,in.right_x,in.right_y,in.move_x,in.move_y,in.move_z};c.direction=in.direction;c.flags=static_cast<std::uint16_t>(in.flags&input::kCommandMask);c.raw=static_cast<std::uint16_t>(in.raw&input::kCommandMask);}
 f.role=r.id;f.episode=r.episode;f.summon_input=r.summon_input;f.cls=r.class_id;f.summon=r.summon_id;f.active=active;f.actor_scale=g.scale.read<float>(a);f.edges=std::move(r.edges);r.edges.clear();
 // A late join borrows only current visual definition; it must not re-launch past projectiles.
 f.command_sequence=r.command.valid()?r.sequence:0;
 if(active)for(unsigned layer:{3u,2u}){net::snap::EventAnimation event;if(lily_events::capture_layer(a,layer,name,event)&&event.generation){f.layer=static_cast<std::uint8_t>(layer);f.animation=event;break;}}
 sink(f);++captured;
}
bool bind(Role& r,const p::SpiritFrame& f){
 auto* pawn=caster(f.context.seat);const auto* s=core::schema::script();r.waiting_for="caster";if(!pawn||!s)return false;
 if(f.context.seat!=own_seat&&f.summon_input){native_lily::Source cursor;if(!native_lily::source(pawn,cursor)||cursor.generation!=f.context.generation||cursor.seq<f.summon_input){r.waiting_for=std::format("summon cursor {}/{} generation {}/{}",cursor.seq,f.summon_input,cursor.generation,f.context.generation);return false;}}
 auto* queue=s->zc_command_queue.object(pawn);auto* cls=load(f.summon);auto* actual_cls=load(f.cls);if(!queue||!cls||!actual_cls)return false;
 reflect::Frame command(g.get_command);if(!command.arg(0,cls).invoke(queue))return false;auto* source=command.ret<O*>();if(!source||!source->IsA(static_cast<ue::UClass*>(g.summon_class.get()))||g.command_pawn.object(source)!=pawn){fail("autonomous summon command ownership/type");return false;}
 reflect::Frame spirit(g.get_spirit);auto* a=spirit.invoke(source)?spirit.ret<O*>():nullptr;
 if(!a){
  // CAS.GetSpirit is a cache populated only by CAS.OnStart. Cold join must acquire the
  // same original equipment/pool instance without replaying Lily's old summon timeline.
  reflect::Frame component(g.component);auto* summoner=component.arg(0,g.summoner_class.get()).invoke(pawn)?component.ret<O*>():nullptr;
  auto* cls=g.spirit_class.object(source);
  using Acquire=O*(__fastcall*)(O*,O* const*);
  auto acquire=reinterpret_cast<Acquire>(preflight::address("SummonerGetSpiritNative"));
  if(summoner&&cls==actual_cls&&acquire){a=acquire(summoner,&cls);if(a&&a->GetClassPrivate()==actual_cls){const auto weak=ObjHandle::of(a);std::memcpy(reinterpret_cast<std::uint8_t*>(source)+g.cached_spirit_offset,&weak,8);}}
 }
 if(!a){r.waiting_for="GetSpirit";return false;} // profile/equipment preload may arrive after the first autonomous frame
 if(a->GetClassPrivate()!=actual_cls||!eligible_impl(a)){fail(std::format("autonomous equipment Spirit identity mismatch: got={} expected={} summon={}",log::utf8(a->GetClassPrivate()->GetPathName()),log::utf8(actual_cls->GetPathName()),log::utf8(cls->GetPathName())));return false;}
 const bool restore=!is_active(a)||(r.bound&&r.episode!=f.episode);
 if(auto* previous=role_of(a);previous&&previous!=&r){release(*previous);previous->frames.clear();}
 release(r);r.actor=ObjHandle::of(a);r.caster=ObjHandle::of(pawn);r.source=ObjHandle::of(source);r.queue=ObjHandle::of(g.spirit_queue.object(a));r.original=g.scale.read<float>(a);
 if(auto w=waiting.find(r.actor);w!=waiting.end()){r.original=w->second.scale;waiting.erase(w);}
 r.seat=f.context.seat;r.generation=f.context.generation;r.episode=f.episode;r.bound=true;r.sequence=0;r.command={};actors[r.actor]=r.id;
 r.elapsed=0;r.started=false;r.last=0;r.world_rate=1;
 pause(a);reflect::Frame(g.clear).invoke(r.queue.get());
 if(restore){auto fn=reinterpret_cast<SpiritStart>(preflight::address("SpiritStartNative"));if(!fn){fail("original Spirit Start unavailable");return false;}replaying=true;fn(a,source);replaying=false;pause(a);}
 r.waiting_for.clear();++bindings;return true;
}
O* follow_anchor(Role& r){
 if(auto* a=r.anchor.get())return a;
 if(!scene_context::create(r.anchor_context))return nullptr;
 r.anchor=r.anchor_context.actor;++anchors_created;return r.anchor.get();
}
void follow_context(Role& r,const p::SpiritFrame& f){
 const auto* s=core::schema::script();if(!s||!r.actor.valid())return;
 auto* follow=r.follow.get();if(!follow){reflect::Frame q(g.component);follow=q.arg(0,g.follow_class.get()).invoke(r.actor.get())?q.ret<O*>():nullptr;r.follow=ObjHandle::of(follow);}
 if(!follow){if(f.has_follow)fail("Host FollowTarget component missing on bound Spirit");return;}
 if(!f.has_follow){r.follow_steps.clear();reflect::Frame(g.follow_clear).invoke(follow);return;}
 auto* anchor=follow_anchor(r);auto* root=anchor?s->actor_root.object(anchor):nullptr;if(!root){fail("FollowTarget context carrier unavailable");return;}
 reflect::Frame(g.set_loc).arg(0,f.follow_position).arg_bool(1,false).arg_bool(3,false).invoke(anchor);reflect::Frame(g.set_rotation).arg(0,f.follow_rotation).arg_bool(1,false).invoke(anchor);reflect::Frame(g.set_scale).arg(0,f.follow_scale).invoke(anchor);*g.scene_velocity.at<V>(root)=f.follow_velocity;
 reflect::Frame current(g.follow_get);if(!current.invoke(follow)){fail("FollowTarget context lookup");return;}
 if(current.ret<O*>()!=root){r.original_target=ObjHandle::of(current.ret<O*>());if(!reflect::Frame(g.follow_set).arg(0,root).arg_bool(1,false).invoke(follow))fail("FollowTarget context assignment");}
}
void __fastcall follow_tick(O* component,float dt,std::int32_t type,void* tick_function){
 if(!ue::on_game_thread()||!g.ready()||sink||!own_seat)return original_follow(component,dt,type,tick_function);
 auto* r=role_of(component?component->GetOuterPrivate():nullptr);
 if(!r||!r->bound||r->follow.get()!=component)return original_follow(component,dt,type,tick_function);
 if(r->updating)return; // never recursively consume this component's network steps
 r->updating=true;
 while(!r->follow_steps.empty()){
  auto step=std::move(r->follow_steps.front());r->follow_steps.pop_front();
  follow_context(*r,step.frame);
  if(step.begin)for(unsigned i=0;i<4;++i)std::memcpy(reinterpret_cast<std::uint8_t*>(component)+g.history[i],step.frame.follow_history.data()+3*i,12);
  original_follow(component,step.dt,type,tick_function);
  ++follow_played;
 }
 r->updating=false;
}
void context(Role& r,const p::SpiritFrame& f){
 auto* a=r.actor.get();const auto* s=core::schema::script();if(!a||!s)return;
 if(auto at=core::location(a)){const float error=std::hypot(at->y-f.context.position[1],at->z-f.context.position[2]);corrections.add(error);if(error>.05f)reflect::Frame(g.set_loc).arg(0,f.context.position).arg_bool(1,false).arg_bool(3,false).invoke(a);}
 auto* move=s->char_movement.object(a);if(move){*g.velocity.at<V>(move)=f.context.velocity;if(s->cm_mode.read<std::uint8_t>(move)!=f.context.mode)reflect::Frame(s->cm_set_mode).arg(0,f.context.mode).arg(1,f.context.custom_mode).invoke(move);}
 auto* loco=g.spirit_locomotion.object(a);if(loco)reflect::Frame(g.face).arg_bool(0,f.context.facing==2).invoke(loco);
 follow_context(r,f);
 auto* q=r.queue.get();if(q){input::InputSnapshot in;const auto& v=f.context.input;in.input_x=v[0];in.input_y=v[1];in.right_x=v[2];in.right_y=v[3];in.move_x=v[4];in.move_y=v[5];in.move_z=v[6];in.direction=f.context.direction;in.flags=f.context.flags;in.raw=f.context.raw;*g.snapshot.at<input::InputSnapshot>(q)=in;}
}
void begin(Role& r,const p::SpiritFrame& f){
 context(r,f);
 if(r.borrowed&&(f.layer!=r.borrowed||!f.edges.empty())){lily_events::apply_layer(r.actor.get(),r.borrowed,{},f.context.t_ms,text);r.borrowed=0;}
 for(const auto& e:f.edges){
  if(e.kind==0){
   auto* cls=load(e.cls);if(!cls){fail("autonomous command class unavailable");return;}
   reflect::Frame(g.clear).invoke(r.queue.get());r.sequence=e.sequence;r.command={};
   replaying=true;reflect::Frame add(g.add);const bool ok=add.arg(0,e.direction).arg(1,e.button).arg(2,cls).invoke(r.queue.get());replaying=false;
   if(!ok||add.ret<std::int32_t>()<0){fail("Host autonomous command could not enter native queue");return;}++starts;
  }else if(r.sequence==e.sequence&&r.command.valid()){
   // Only an exact still-running command may be cancelled; a naturally ended pooled command is not restarted.
   if(e.reason==0){reflect::Frame(g.request_end).arg_bool(0,false).invoke(r.command.get());original_update(r.queue.get(),0);}
   else reflect::Frame(g.clear).invoke(r.queue.get());
   r.command={};++ends;
  }
 }
 if(f.layer&&f.animation.generation&&f.edges.empty()&&(!f.command_sequence||r.sequence!=f.command_sequence)){if(!lily_events::apply_layer(r.actor.get(),f.layer,f.animation,f.context.t_ms,text))fail("autonomous non-command animation context");r.borrowed=f.layer;}
}
void __fastcall update(O* queue,float dt){
 if(!ue::on_game_thread()||!g.ready())return original_update(queue,dt);
 // Native immediate enqueue synchronously calls UpdateCommands(0). That inner call belongs to
 // the original enqueue, not another consumption of the still-current network frame.
 if(replaying)return original_update(queue,dt);
 auto* pawn=g.queue_pawn.object(queue);auto* r=role_of(pawn);
 if(sink){if(r&&is_active(pawn)&&(dt>0||world_dt(pawn)*g.scale.read<float>(pawn)<.000001f))capture(*r,dt,true);return original_update(queue,dt);}
 if(!own_seat||!eligible_impl(pawn))return original_update(queue,dt);
 if(!r||!r->bound)return; // a local summon has not been accepted by Host yet
 if(r->updating)return original_update(queue,dt);
 struct Updating { bool& flag;explicit Updating(bool& f):flag(f){flag=true;}~Updating(){flag=false;} } scope(r->updating);
 if(r->frames.empty()){*g.scale.at<float>(pawn)=0;++starved;return;}
 float queued=-r->elapsed;for(const auto& f:r->frames)queued+=f.context.step;buffer.add(queued*1000);
 if(!r->started){if(queued<.05f){*g.scale.at<float>(pawn)=0;return;}r->started=true;}
 const auto now=clock::now_us();const float physical=r->last?std::clamp(static_cast<float>(now-r->last)/1e6f,0.f,.05f):1/60.f;r->last=now;
 r->rate=std::clamp(1.f+(queued-.05f)*.8f,.9f,1.1f);float budget=physical*r->rate;float total_world=0;
 unsigned steps=0;
 while(budget>0&&!r->frames.empty()&&steps++<32){
  auto& f=r->frames.front();auto& c=f.context;const auto time=c.t_ms+r->elapsed*1000;
  if(f.episode!=r->episode||c.generation!=r->generation)break; // rebind at the next frame-pump boundary
  if(!causality::ready(time))break;
  const float local_delta=world_dt(pawn);const float world_rate=local_delta>0?c.world_dt/c.step*physical*r->rate/local_delta:0;
  if(!f.decision_only)r->world_rate=world_rate;
  causality::advance(time,r->world_rate);
  if(r->elapsed==0){if(!f.active){reflect::Frame(g.disappear).invoke(pawn);r->frames.pop_front();r->bound=false;++removed;break;}begin(*r,f);}
  if(f.decision_only){r->frames.pop_front();++played;continue;}
  const float span=std::min(budget,c.step-r->elapsed);if(span<=0)break;
  const bool first=r->elapsed==0;
  original_update(queue,c.dt*(span/c.step));const float component_delta=c.world_dt*(span/c.step)*f.actor_scale;total_world+=component_delta;
  if(f.has_follow){if(r->follow_steps.size()>=64){fail("autonomous FollowTarget step capacity");break;}r->follow_steps.push_back({f,component_delta,first});}
  r->elapsed+=span;budget-=span;
  if(r->elapsed>=c.step-.000001f){r->frames.pop_front();r->elapsed=0;++played;}
 }
 const float local=world_dt(pawn);*g.scale.at<float>(pawn)=local>0?std::clamp(total_world/local,0.f,1000.f):0;
}
bool install(){
 if(g.ready())return true;
 auto* spirit=reflect::find_class(L"/Script/Zenith.ZenithSpirit");reflect::Resolver sp("autonomous Spirit",spirit);g.summoner=sp.func(L"GetSummoner",0);g.active=sp.func(L"IsActive",0);g.disappear=sp.func(L"ForceDisappear",0);
 g.spirit_queue=sp.prop(L"CommandQueueComponent",reflect::Kind::Object);g.spirit_locomotion=sp.prop(L"LocomotionComponent",reflect::Kind::Object);
 reflect::Resolver pawn("autonomous Pawn",reflect::find_class(L"/Script/Engine.Pawn"));g.ai_class=pawn.prop(L"AIControllerClass",reflect::Kind::Class);
 reflect::Resolver cq("autonomous queue",reflect::find_class(L"/Script/Zenith.CommandQueueComponent"));g.queue_pawn=cq.prop(L"Pawn",reflect::Kind::Object);g.snapshot=cq.prop(L"CurrentInputSnapshot",reflect::Kind::Struct,40);g.get_command=cq.func(L"GetOrCreateCommandAction",1);g.add=cq.func(L"AddNewImmediateCommand",3);g.clear=cq.func(L"ClearCommandBuffer",0);auto tick=cq.func(L"UpdateCommands",1);
 reflect::Resolver ca("autonomous command",reflect::find_class(L"/Script/Zenith.CommandAction"));g.command_pawn=ca.prop(L"Pawn",reflect::Kind::Object);g.direction=ca.func(L"GetInvokedByDirectionInputType",0);g.button=ca.func(L"GetInvokedByInputType",0);
 g.request_end=ca.func(L"RequestEnd",1);
 auto* summon_class=reflect::find_class(L"/Script/Zenith.CommandActionSummon");g.summon_class=ObjHandle::of(summon_class);reflect::Resolver summon("autonomous summon",summon_class);g.get_spirit=summon.func(L"GetSpirit",0);g.spirit_class=summon.prop(L"SpiritClass",reflect::Kind::Class);
 const auto cached=binary_checks::offset("CommandActionSummon.cachedSpirit");if(!cached||*cached+8>reflect::properties_size(summon_class)||!preflight::address("SummonerGetSpiritNative")){fail("autonomous cold summon layout/entry unavailable");return false;}g.cached_spirit_offset=static_cast<int>(*cached);g.summoner_class=ObjHandle::of(reflect::find_class(L"/Script/Zenith.SummonerComponent"));if(!g.summoner_class.valid())return false;
 reflect::Resolver ai("autonomous AI",reflect::find_class(L"/Script/AIModule.AIController"));g.brain=ai.prop(L"BrainComponent",reflect::Kind::Object);
 reflect::Resolver brain("autonomous Brain",reflect::find_class(L"/Script/AIModule.BrainComponent"));g.running=brain.func(L"IsRunning",0);g.stop_logic=brain.func(L"StopLogic",1);
 reflect::Resolver a("autonomous actor",reflect::find_class(L"/Script/Engine.Actor"));g.scale=a.prop(L"CustomTimeDilation",reflect::Kind::Float,4);g.loc=a.func(L"K2_GetActorLocation",0);g.set_loc=a.func(L"K2_SetActorLocation",4);g.rotation=a.func(L"K2_GetActorRotation",0);g.set_rotation=a.func(L"K2_SetActorRotation",2);
 g.component=a.func(L"GetComponentByClass",1);g.hidden=a.func(L"SetActorHiddenInGame",1);g.collision=a.func(L"SetActorEnableCollision",1);g.tick=a.func(L"SetActorTickEnabled",1);
 g.set_scale=a.func(L"SetActorScale3D",1);
 auto* ft=reflect::find_class(L"/Script/Zenith.FollowTargetComponent");reflect::Resolver follow("autonomous FollowTarget",ft);g.follow_get=follow.func(L"GetFollowTarget",0);g.follow_set=follow.func(L"SetFollowTarget",2);g.follow_clear=follow.func(L"ClearFollowTarget",0);g.follow_class=ObjHandle::of(ft);g.anchor_class=ObjHandle::of(reflect::find_class(L"/Script/Engine.TargetPoint"));
 const std::array<std::string_view,4> history_names{"FollowTarget.blend","FollowTarget.phase","FollowTarget.target","FollowTarget.previous"};
 for(unsigned i=0;i<4;++i){auto offset=binary_checks::offset(history_names[i]);if(!offset||*offset+12>reflect::properties_size(ft)){fail("FollowTarget numeric history layout");return false;}g.history[i]=static_cast<int>(*offset);}
 reflect::Resolver comp("autonomous component clock",reflect::find_class(L"/Script/Engine.ActorComponent"));g.follow_enabled=comp.func(L"IsComponentTickEnabled",0);
 reflect::Resolver scene("autonomous follow context",reflect::find_class(L"/Script/Engine.SceneComponent"));g.scene_position=scene.func(L"K2_GetComponentLocation",0);g.scene_rotation=scene.func(L"K2_GetComponentRotation",0);g.scene_velocity=scene.prop(L"ComponentVelocity",reflect::Kind::Struct,12);
 g.scene_scale=scene.func(L"K2_GetComponentScale",0);
 reflect::Resolver m("autonomous motion",reflect::find_class(L"/Script/Engine.CharacterMovementComponent"));g.velocity=m.prop(L"Velocity",reflect::Kind::Struct,12);
 reflect::Resolver l("autonomous facing",reflect::find_class(L"/Script/Zenith.LocomotionComponent"));g.face=l.func(L"SetFacing",1);
 reflect::Resolver gs("autonomous world delta",reflect::find_class(L"/Script/Engine.GameplayStatics"));g.world_delta=gs.func(L"GetWorldDeltaSeconds",1);g.statics=ObjHandle::of(reflect::find_object(L"/Script/Engine.Default__GameplayStatics"));
 for(auto* v:{&sp,&pawn,&cq,&ca,&summon,&ai,&brain,&a,&m,&l,&gs,&follow,&scene,&comp})if(!v->ok()){fail(v->failure());return false;}
 auto* exec=reinterpret_cast<const std::uint8_t*>(tick.fn->GetFuncPtr());if(!exec||exec[0x70]!=0xE8){fail("autonomous CQ exec call shape");return false;}std::int32_t rel=0;std::memcpy(&rel,exec+0x71,4);auto* target=const_cast<std::uint8_t*>(exec+0x75+rel);
 if(target!=preflight::address("CommandQueueUpdateNative")){fail("autonomous CQ target identity");return false;}
 const auto offset=binary_checks::offset("ZenithSpirit.sourceCommand");if(!offset||*offset+8>reflect::properties_size(spirit)){fail("autonomous source layout unavailable");return false;}g.source_offset=static_cast<int>(*offset);
 if(!original_update&&!hook::install_address("CommandQueueUpdateNative",target,reinterpret_cast<void*>(&update),reinterpret_cast<void**>(&original_update),"reflect")){fail("autonomous CQ hook unavailable");return false;}
 if(!original_follow&&!hook::install_manifest("FollowTargetTickNative",reinterpret_cast<void*>(&follow_tick),reinterpret_cast<void**>(&original_follow))){fail("autonomous FollowTarget hook unavailable");return false;}
 g.spirit=ObjHandle::of(spirit);g.ai=ObjHandle::of(reflect::find_class(L"/Script/Zenith.ZenithAIBaseController"));return g.ready();
}
}
bool eligible(O* a){return g.ready()&&eligible_impl(a);}
O* summoner(O* a){return g.ready()?get_summoner(a):nullptr;}
bool source(O* a,native_lily::Source& out){if(auto* r=owner_role(a)){out={r->seat,r->generation,r->sequence,true,1};return true;}return false;}
bool owns(O* a){return !sink&&own_seat&&owner_role(a)!=nullptr;}
void summoned(O* a,O* command){if(sink)register_host(a,command,true);}
void command_started(O* command,O* pawn){if(auto* r=role_of(pawn)){
 if(sink){if(next_sequence==UINT32_MAX){fail("autonomous command sequence exhausted");return;}r->sequence=++next_sequence;reflect::Frame d(g.direction),b(g.button);if(!d.invoke(command)||!b.invoke(command))return;auto cls=name(log::utf8(command->GetClassPrivate()->GetPathName()));if(r->edges.size()>=16){fail("autonomous command edge capacity");return;}r->edges.push_back({r->sequence,cls,0,d.ret<std::uint8_t>(),b.ret<std::uint8_t>(),0});r->command=ObjHandle::of(command);capture(*r,0,true,true);}
 else if(!replaying){++unexpected_starts;return;}
 r->command=ObjHandle::of(command);
}}
void command_ended(O* command,O* pawn,std::uint8_t reason){if(auto* r=role_of(pawn);r&&r->command==ObjHandle::of(command)){
 if(sink&&r->sequence){if(r->edges.size()>=16){fail("autonomous command edge capacity");return;}r->edges.push_back({r->sequence,name(log::utf8(command->GetClassPrivate()->GetPathName())),1,0,0,reason});capture(*r,0,true,true);}r->command={};
}}
bool host_tick(FrameSink s,native_lily::Name n){sink=s;name=n;if(!install())return false;
 for(const auto& e:registry::bucket(registry::Kind::Spirit))if(auto* a=e.handle.get();eligible_impl(a)&&is_active(a)){
  if(!role_of(a)){ObjHandle source;std::memcpy(&source,reinterpret_cast<std::uint8_t*>(a)+g.source_offset,8);if(auto* cmd=source.get())register_host(a,cmd,false);}
 }
 std::erase_if(roles,[](auto& v){auto& r=v.second;if(!r.actor.valid()||!r.caster.valid()||!r.source.valid()||caster(r.seat)!=r.caster.get()){capture(r,0,false);release(r);return true;}if(!is_active(r.actor.get())){capture(r,0,false);release(r);return true;}return false;});return failure.empty();
}
void receive(const p::SpiritFrame& f){if(roles.size()>=128&&!roles.contains(f.role)){fail("autonomous replica capacity");return;}auto& r=roles[f.role];r.id=f.role;
 if(r.frame>=f.context.seq){fail("autonomous frame sequence regression");return;}r.frame=f.context.seq;
 if(r.frames.size()>=120){fail(std::format("autonomous playback capacity role={} bound={} active={} wait={} source={} frame={} elapsed={}",r.id,r.bound,is_active(r.actor.get()),r.waiting_for,f.summon_input,f.context.seq,r.elapsed));return;}r.frames.push_back(f);++received;
}
bool client_tick(std::uint16_t seat,std::uint16_t map,native_lily::Text t){own_seat=seat;current_map=map;text=t;sink=nullptr;if(!install())return false;
 for(auto& [_,r]:roles)if(r.bound&&(!r.actor.valid()||!r.caster.valid()||!r.source.valid()||r.caster.get()!=caster(r.seat))){if(auto* a=r.actor.get())reflect::Frame(g.disappear).invoke(a);release(r);r.frames.clear();}
 for(auto& [_,r]:roles)if(r.bound&&r.actor.valid()&&!is_active(r.actor.get())&&!r.frames.empty()&&!r.frames.back().active){release(r);r.frames.clear();++removed;}
 for(auto& [_,r]:roles)if(!r.frames.empty()){
  if(!r.bound&&!r.frames.back().active){r.frames.clear();continue;}
  const auto& f=r.frames.front();if((!r.bound||!r.actor.valid()||r.episode!=f.episode||r.generation!=f.context.generation||r.caster.get()!=caster(f.context.seat))&&f.active)bind(r,f);
  if(r.bound){pause(r.actor.get());if(!is_active(r.actor.get())&&!r.frames.empty()&&r.frames.back().active){bind(r,f);}}
 }
 for(const auto& e:registry::bucket(registry::Kind::Spirit))if(auto* a=e.handle.get();eligible_impl(a)&&is_active(a)&&!role_of(a)){
  if(!get_summoner(a))continue;pause(a);const auto h=ObjHandle::of(a);if(waiting.size()>=128&&!waiting.contains(h)){fail("autonomous pending role capacity");return false;}auto [i,fresh]=waiting.try_emplace(h,Waiting{g.scale.read<float>(a),clock::now_us()});*g.scale.at<float>(a)=0;
  if(clock::now_us()-i->second.since>3'000'000){reflect::Frame(g.disappear).invoke(a);*g.scale.at<float>(a)=i->second.scale;waiting.erase(i);++removed;}
 }
 std::erase_if(waiting,[](const auto& v){return !v.first.valid();});
 std::erase_if(roles,[](auto& v){auto& r=v.second;if(r.frames.empty()&&!r.bound){release(r);return true;}return false;});return failure.empty();
}
void reset(){for(auto& [_,r]:roles)release(r);for(const auto& [a,w]:waiting)if(auto* actor=a.get())*g.scale.at<float>(actor)=w.scale;roles.clear();actors.clear();waiting.clear();classes.clear();sink=nullptr;name=nullptr;text=nullptr;own_seat=0;current_map=0;g={};failure.clear();}
std::string fault(){return failure;}
std::string describe(){std::size_t queued=0,pending=0;for(const auto& [_,r]:roles){queued+=r.frames.size();pending+=r.follow_steps.size();}return std::format("autonomous: roles={} waiting={} captured={} received={} consumed={} queued={} commands={}/{} bindings={} brains={} removed={} starved={} anchors={}/{} follow={}/{} unexpectedStarts={} fault='{}'\n",roles.size(),waiting.size(),captured,received,played,queued,starts,ends,bindings,paused_brains,removed,starved,anchors_created,anchors_removed,follow_played,pending,unexpected_starts,failure);}
}
