#include "coop/causal_facts.h"
#include "coop/session.h"
#include "coop/feedback_context.h"
#include "core/player.h"
#include "core/schema.h"
#include "engine/reflect.h"
#include "engine/scene_context.h"
#include "base/clock.h"
#include "base/log.h"
#include <Unreal/Hooks/Hooks.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <unordered_map>

namespace elmp::coop::causality {
namespace {
namespace p = net::proto;
using ue::UObject;
using V3 = std::array<float,3>;
HostApi g_host;
ClientApi g_client;
std::string g_fault;
std::vector<p::Cause> g_out;
std::uint64_t g_batch = 0;
std::uint32_t g_fence = 0;
double g_time = 0;
float g_rate = 1;
std::int64_t g_wait = 0;
RC::Unreal::Hook::GlobalCallbackId g_post{};
bool g_post_live = false;
UObject* g_setting_target = nullptr;
struct Root {
    native_lily::Source source;
    std::uint64_t wire = 0;
    std::string cls, invoker, key;
    std::uint16_t ordinal = 0, map = 0;
    float offset = 0;
    p::Cause seed;
    bool homing = false;
    std::int64_t ended = 0;
};
struct Node {
    scene_context::Context target_context;
    ObjHandle actor, invoker, component, target, target_actor;
    ability::Identity id;
    unsigned query = 0;
    std::size_t pending_query = ~std::size_t{};
    std::uint64_t batch = 0;
    std::uint8_t reason = 0;
    float actor_scale = 1;
    float original_scale = 1;
    bool scaled = false;
};
struct Counter { native_lily::Source source; std::int64_t touched = 0; std::unordered_map<std::string,unsigned> by_class; };
std::unordered_map<std::string,Counter> g_ordinals;
std::unordered_map<std::uint64_t,Root> g_roots;
std::unordered_map<std::uint64_t,Node> g_nodes;
std::unordered_map<std::string,p::Cause> g_declarations;
struct Record { p::Cause fact; bool used = false; };
std::vector<Record> g_records;
std::unordered_map<std::string,std::deque<p::Cause>> g_samples;
std::uint64_t g_bound = 0, g_overridden = 0, g_nulls = 0, g_deferred = 0, g_authorized = 0, g_forced = 0, g_contexts = 0, g_retired = 0;
reflect::FuncRef f_actor_pos, f_actor_set, f_actor_rot, f_actor_set_rot, f_scene_pos, f_scene_vel, f_scene_set, f_get_target, f_set_target, f_stop, f_world_delta;
reflect::PropRef prop_velocity, prop_dilation;
ObjHandle g_statics;
void fail(std::string s) { if(g_fault.empty()) g_fault=std::move(s); }
bool host() { return g_host.name != nullptr; }
std::uint32_t now_ms() { return static_cast<std::uint32_t>(clock::now_us()/1000); }
std::string child_key(std::uint64_t root,const p::AbilityPath& path) {
    auto s=std::to_string(root);for(unsigned i=0;i<path.depth;++i)s+=std::format("/{}:{}",path.entries[i],path.occurrences[i]);return s;
}
std::string root_key(native_lily::Source s,std::string_view cls,std::string_view inv,unsigned ordinal) {
    return std::format("{}:{}:{}:{}:{}:{}:{}",s.domain,s.seat,s.generation,s.seq,cls,inv,ordinal);
}
std::string class_name(UObject* o) { return o?log::utf8(o->GetClassPrivate()->GetPathName()):std::string{}; }
bool vec(reflect::FuncRef f,UObject* o,V3& v) { reflect::Frame q(f);if(!o||!q.invoke(o))return false;v=q.ret<V3>();return std::all_of(v.begin(),v.end(),[](float n){return std::isfinite(n);}); }
std::string relative(UObject* object,UObject* owner) {
    std::string path;
    for(int i=0;object&&object!=owner&&i<16;++i){path=log::utf8(object->GetName())+(path.empty()?"":"."+path);object=object->GetOuterPrivate();}
    return object==owner?path:std::string{};
}
UObject* component_owner(UObject* comp) {
    static reflect::FuncRef f=reflect::Resolver("causal component owner",reflect::find_class(L"/Script/Engine.ActorComponent")).func(L"GetOwner",0);
    reflect::Frame q(f);return comp&&q.invoke(comp)?q.ret<UObject*>():nullptr;
}
Node* node(const ability::Identity& id) { auto i=g_nodes.find(id.token);return i!=g_nodes.end()&&i->second.actor.valid()&&ability::activation(i->second.actor.get())==id.token?&i->second:nullptr; }
Root* root(const ability::Identity& id) {auto i=g_roots.find(id.root);return i==g_roots.end()?nullptr:&i->second;}
bool current_view(const Root& r){if(!host())return true;std::uint16_t map=0;float offset=0;return g_host.place&&g_host.place(r.source.seat,map,offset)&&map==r.map;}
void bind(Root& r) {
    if(host()||r.wire)return;
    const auto i=g_declarations.find(r.key);if(i==g_declarations.end())return;
    r.wire=i->second.root;r.map=i->second.map;r.seed=i->second;g_declarations.erase(i);++g_bound;
}
p::Cause base(p::CauseKind kind,const Node& n,Root& r) {
    p::Cause f;f.kind=kind;f.domain=r.source.domain;f.seat=r.source.seat;f.generation=r.source.generation;f.input=r.source.seq;f.root=r.wire;f.map=r.map;f.t_ms=now_ms();f.path=n.id.path;
    f.cls=g_host.name(n.id.cls);f.ordinal=r.ordinal;if(auto* a=n.actor.get())f.actor_scale=prop_dilation.read<float>(a);return f;
}
void emit(p::Cause f) { if(g_out.size()>=p::kCauses)return fail("causal frame exceeds fact capacity");g_out.push_back(std::move(f)); }
bool reference(UObject* actor,p::Cause& f,Root& r) {
    if(!actor)return true;
    if(!g_host.reference||!g_host.reference(actor,f,r.map)||!vec(f_actor_pos,actor,f.actor_position)){fail("causal target is not a mapped enemy in this view");return false;}
    f.actor_position[1]-=r.offset;return true;
}
bool context(UObject* component,p::Cause& f,Root& r) {
    if(!component){f.ref_kind=0;f.ref_id=0;f.ref_generation=0;f.component=0;return true;}
    auto* owner=component_owner(component);if(!owner||!reference(owner,f,r))return false;
    const auto path=relative(component,owner);
    if(path.empty()||!vec(f_scene_pos,component,f.target_position)||!vec(f_scene_vel,component,f.target_velocity)){fail("causal target component context unavailable");return false;}
    f.component=g_host.name(path);f.target_position[1]-=r.offset;return f.component!=0;
}
UObject* resolve(const p::Cause& f) { return f.ref_kind&&g_client.actor?g_client.actor(f.ref_kind,f.ref_id,f.ref_generation):nullptr; }
bool matches(const p::Cause& f,const Node& n,const Root& r) {
    const auto* cls=g_client.text?g_client.text(f.cls):nullptr;
    return f.root==r.wire&&f.path==n.id.path&&cls&&*cls==n.id.cls;
}
Record* expected_end(const Node& n,const Root& r) {
    for(auto& e:g_records)if(!e.used&&e.fact.kind==p::CauseKind::End&&matches(e.fact,n,r))return &e;return nullptr;
}
void apply_context(Node& n,const p::Cause& f) {
    if(!f.ref_kind){auto* previous=g_setting_target;g_setting_target=n.component.get();reflect::Frame(f_set_target).arg(0,static_cast<UObject*>(nullptr)).invoke(n.component.get());g_setting_target=previous;n.target={};n.target_actor={};return;}
    auto* r=root(n.id);
    if(n.target_context.actor.valid()||(r&&r->source.domain==1&&!resolve(f)&&f.component)){
        if(!scene_context::create(n.target_context,true)||!scene_context::pose(n.target_context,f.actor_position,{},V3{1,1,1},f.target_velocity)||!scene_context::aim(n.target_context,f.target_position,f.target_velocity)){fail("historical Homing target context unavailable");return;}
        if(n.target!=n.target_context.aim){auto* previous=g_setting_target;g_setting_target=n.component.get();reflect::Frame(f_set_target).arg(0,n.target_context.aim.get()).invoke(n.component.get());g_setting_target=previous;}
        n.target=n.target_context.aim;n.target_actor=n.target_context.actor;++g_contexts;return;
    }
    auto* actor=resolve(f);auto* component=n.target.get();
    if(!actor||!component||component_owner(component)!=actor){fail("causal target component no longer matches authoritative target");return;}
    if(auto pos=core::location(actor);pos&&std::hypot(pos->y-f.actor_position[1],pos->z-f.actor_position[2])>0.05f)
        reflect::Frame(f_actor_set).arg(0,f.actor_position).arg_bool(1,false).arg_bool(3,false).invoke(actor);
    V3 at;
    if(vec(f_scene_pos,component,at)&&std::hypot(at[1]-f.target_position[1],at[2]-f.target_position[2])>0.05f)
        reflect::Frame(f_scene_set).arg(0,f.target_position).arg_bool(1,false).arg_bool(3,false).invoke(component);
    std::memcpy(prop_velocity.at<void>(component),f.target_velocity.data(),12);++g_contexts;
}
const p::Cause* sample(const Node& n,const Root& r,p::Cause& out) {
    const auto it=g_samples.find(child_key(r.wire,n.id.path));if(it==g_samples.end()||it->second.empty())return nullptr;
    const auto& a=it->second;const p::Cause* lo=&a.front();const p::Cause* hi=lo;
    for(const auto& s:a){if(s.t_ms<=g_time)lo=&s;hi=&s;if(s.t_ms>g_time)break;}
    out=*lo;
    if(hi!=lo&&hi->t_ms>lo->t_ms&&hi->ref_kind==lo->ref_kind&&hi->ref_id==lo->ref_id&&hi->component==lo->component){
        const float k=static_cast<float>(std::clamp((g_time-lo->t_ms)/(hi->t_ms-lo->t_ms),0.0,1.0));
        for(unsigned i=0;i<3;++i){out.actor_position[i]+=k*(hi->actor_position[i]-lo->actor_position[i]);out.target_position[i]+=k*(hi->target_position[i]-lo->target_position[i]);out.target_velocity[i]+=k*(hi->target_velocity[i]-lo->target_velocity[i]);}
    }return &out;
}
void post_tick() {
    if(!g_host.name&&!g_client.text)return;
    struct Stop { ObjHandle actor; std::uint64_t token; std::uint8_t reason; };
    std::vector<Stop> stops;
    for(auto& [_,n]:g_nodes)if(auto* r=root(n.id);!host()&&r&&r->homing&&r->wire&&n.id.active){
        if(auto* e=expected_end(n,*r);e&&e->fact.t_ms<=g_time)stops.push_back({n.actor,n.id.token,e->fact.reason});
    }
    for(const auto& pending:stops)if(auto* a=pending.actor.get()){ability::Identity id;if(ability::identity(a,id)&&id.active&&id.token==pending.token){ability::NativeScope scope(a);if(reflect::Frame(f_stop).arg(0,pending.reason).invoke(a))++g_forced;}}
    const auto now=clock::now_us();
    std::vector<std::uint64_t> retired;
    for(auto& [token,r]:g_roots){if(ability::has_active_family(token)){r.ended=0;continue;}if(!r.ended)r.ended=now;if(now-r.ended>2'000'000)retired.push_back(token);}
    for(auto token:retired){const auto wire=g_roots[token].wire;g_roots.erase(token);std::erase_if(g_nodes,[&](auto& n){if(n.second.id.root!=token)return false;scene_context::destroy(n.second.target_context);return true;});std::erase_if(g_records,[&](const auto& e){return e.fact.root==wire;});std::erase_if(g_samples,[&](const auto& v){return v.first==std::to_string(wire)||v.first.starts_with(std::to_string(wire)+"/");});++g_retired;}
    std::erase_if(g_ordinals,[&](const auto& p){
        const auto& c=p.second;if(now-c.touched<2'000'000||retained_source(c.source))return false;
        return std::none_of(g_roots.begin(),g_roots.end(),[&](const auto& r){const auto& s=r.second.source;return s.seat==c.source.seat&&s.generation==c.source.generation&&s.seq==c.source.seq&&s.domain==c.source.domain;});
    });
    if(g_records.size()>8192||g_declarations.size()>2048||g_nodes.size()>4096||g_roots.size()>1024)fail("causal replay ledger capacity");
    if(!host())for(const auto& [_,f]:g_declarations)if(g_time>f.t_ms+1000.0)fail("Host root was not consumed by its original native activation");
    if(!host())for(const auto& e:g_records)if(!e.used&&e.fact.kind==p::CauseKind::Query&&g_time>e.fact.t_ms+1000.0)fail("Host query was not consumed by its original native activation");
}
}
void host_api(HostApi a){g_host=a;g_client={};}
void client_api(ClientApi a){g_client=a;g_host={};}
std::vector<p::Cause> take(){auto out=std::move(g_out);g_out.clear();++g_batch;return out;}
bool root_active(std::uint64_t wire){return ability::has_active_family(wire);}
bool receive(const p::LilyCauses& b) {
    for(const auto& f:b.facts){
        if(!g_client.text||!g_client.text(f.cls)){fail("causal class name not defined");return false;}
        if(f.kind==p::CauseKind::Root){const auto* inv=g_client.text(f.invoker);if(!inv)return false;const native_lily::Source s{f.seat,f.generation,f.input,false,f.domain};const auto key=root_key(s,*g_client.text(f.cls),*inv,f.ordinal);if(g_declarations.contains(key))return false;g_declarations[key]=f;}
        else if(f.kind==p::CauseKind::Target){auto& q=g_samples[child_key(f.root,f.path)];if(q.size()==128)q.pop_front();q.push_back(f);}
        else {g_records.push_back({f});if(f.kind==p::CauseKind::Query&&f.component){auto seed=f;seed.kind=p::CauseKind::Target;seed.min=seed.range=0;auto& q=g_samples[child_key(f.root,f.path)];if(q.size()==128)q.pop_front();q.push_back(seed);}}
    }
    g_fence=b.fence_ms;return true;
}
bool ready(double time) {
    if(!g_client.text)return true;
    bool ok=g_fence&&time<=g_fence+1;
    const p::Cause* missing=nullptr;
    for(const auto& e:g_records)if(!e.used&&e.fact.kind==p::CauseKind::Query&&e.fact.ref_kind&&e.fact.t_ms<=time+1&&!resolve(e.fact)&&!(e.fact.domain==1&&e.fact.component)){ok=false;missing=&e.fact;break;}
    if(ok){g_wait=0;return true;}
    if(!g_wait)g_wait=clock::now_us();else if(clock::now_us()-g_wait>1'000'000)fail(std::format("causal {} not ready: playback={:.3f} fence={} root={} target={}:{} generation={} domain={}",missing?"target":"fence",time,g_fence,missing?missing->root:0,missing?missing->ref_kind:0,missing?missing->ref_id:0,missing?missing->ref_generation:0,missing?missing->domain:0));return false;
}
void advance(double time,float rate){g_time=time;g_rate=rate;if(g_client.motion)g_client.motion(time);for(auto& [_,n]:g_nodes)if(auto* r=root(n.id);r&&r->homing&&n.id.active)if(auto* a=n.actor.get();a&&ability::activation(a)==n.id.token){p::Cause f;if(sample(n,*r,f))n.actor_scale=f.actor_scale;*prop_dilation.at<float>(a)=g_rate*n.actor_scale;n.scaled=true;}}
double playback_ms(){return g_time;}
bool target_owned(UObject* a){for(const auto& [_,n]:g_nodes)if(n.id.active&&n.target_actor.get()==a)return true;return false;}
bool allow_stop(UObject*,const ability::Identity& id,std::uint8_t reason){
    if(host())return true;
    auto* n=node(id);auto* r=root(id);if(!n||!r||!r->wire||!r->homing)return true;
    auto* e=expected_end(*n,*r);if(reason==3||(e&&e->fact.reason==reason&&e->fact.t_ms<=g_time+1)){
        if(n->scaled)if(auto* a=n->actor.get()){*prop_dilation.at<float>(a)=n->original_scale;n->scaled=false;}
        ++g_authorized;return true;
    }++g_deferred;return false;
}
bool event_due(std::uint64_t wire,std::uint32_t time){if(!wire)return true;for(const auto& [_,r]:g_roots)if(r.wire==wire)return time<=g_time;return false;}
std::uint64_t current_root(){
    ability::Identity id;
    if(feedback_context::effect_scope()){
        auto* self=feedback_context::effect_source();
        for(unsigned depth=0;self&&depth<32;++depth,self=self->GetOuterPrivate())
            if(ability::identity(self,id)){auto* r=root(id);return r?r->wire:0;}
        return 0;
    }
    if(!ability::current(id))return 0;auto* r=root(id);return r?r->wire:0;
}
std::uint16_t source_seat(std::uint64_t wire){for(const auto& [_,r]:g_roots)if(r.wire==wire)return r.source.seat;return 0;}
std::uint64_t wire_root(std::uint64_t local){auto it=g_roots.find(local);return it==g_roots.end()?0:it->second.wire;}
std::string fault(){return g_fault;}
namespace facts {
bool install(){
    reflect::Resolver a("causal actor",reflect::find_class(L"/Script/Engine.Actor"));f_actor_pos=a.func(L"K2_GetActorLocation",0);f_actor_set=a.func(L"K2_SetActorLocation",4);f_actor_rot=a.func(L"K2_GetActorRotation",0);f_actor_set_rot=a.func(L"K2_SetActorRotation",2);prop_dilation=a.prop(L"CustomTimeDilation",reflect::Kind::Float,4);
    reflect::Resolver s("causal component",reflect::find_class(L"/Script/Engine.SceneComponent"));f_scene_pos=s.func(L"K2_GetComponentLocation",0);f_scene_vel=s.func(L"GetComponentVelocity",0);f_scene_set=s.func(L"K2_SetWorldLocation",4);prop_velocity=s.prop(L"ComponentVelocity",reflect::Kind::Struct,12);
    reflect::Resolver h("causal homing",reflect::find_class(L"/Script/Zenith.HomingComponent"));f_get_target=h.func(L"GetTarget",0);f_set_target=h.func(L"SetTarget",1);
    f_stop=reflect::Resolver("causal stop",reflect::find_class(L"/Script/Zenith.Ability")).func(L"Stop",1);
    if(!a.ok()||!s.ok()||!h.ok()||!f_stop.ok())return false;
    if(!g_post_live){RC::Unreal::Hook::FCallbackOptions o;o.OwnerModName=STR("ELMP");o.HookName=STR("coop.causality.post");g_post=RC::Unreal::Hook::RegisterEngineTickPostCallback([](auto&,RC::Unreal::UEngine*,float,bool){post_tick();},o);g_post_live=true;}
    return true;
}
void clear(){for(auto& [_,n]:g_nodes){scene_context::destroy(n.target_context);if(n.scaled)if(auto* a=n.actor.get();a&&ability::activation(a)==n.id.token)*prop_dilation.at<float>(a)=n.original_scale;}
    if(g_post_live){RC::Unreal::Hook::UnregisterCallback(g_post);g_post_live=false;}g_host={};g_client={};g_fault.clear();g_out.clear();++g_batch;g_roots.clear();g_nodes.clear();g_declarations.clear();g_records.clear();g_samples.clear();g_ordinals={};g_fence=0;g_time=0;g_wait=0;g_rate=1;}
void launch(UObject* a,UObject* inv,const ability::Identity& id,native_lily::Source src){
    if(!g_host.name&&!g_client.text)return;
    const bool is_root=id.token==id.root;
    if(!id.supported){if(g_roots.contains(id.root))fail("native descendant has no proven source path");return;}
    if(is_root){
        if(!src.seat||!src.generation||!src.seq||!src.command_tick)return;
        const auto source_key=std::format("{}:{}:{}:{}",src.domain,src.seat,src.generation,src.seq);
        if(!g_ordinals.contains(source_key)&&g_ordinals.size()>=2048){fail("causal source ordinal capacity");return;}
        auto& count=g_ordinals[source_key];count.source=src;count.touched=clock::now_us();
        Root r;r.source=src;r.cls=id.cls;r.invoker=class_name(inv);r.ordinal=static_cast<std::uint16_t>(++count.by_class[r.cls+"|"+r.invoker]);r.key=root_key(src,r.cls,r.invoker,r.ordinal);
        if(host()){r.wire=id.root;if(!g_host.place||!g_host.place(src.seat,r.map,r.offset)){fail("causal source map unavailable");return;}}
        else bind(r);
        g_roots[id.root]=r;
    }
    auto* r=root(id);if(!r)return;
    if(!host()&&is_root&&r->wire&&r->source.domain==1){reflect::Frame(f_actor_set).arg(0,r->seed.actor_position).arg_bool(1,false).arg_bool(3,false).invoke(a);reflect::Frame(f_actor_set_rot).arg(0,r->seed.actor_rotation).arg_bool(1,true).invoke(a);}
    Node n;n.actor=ObjHandle::of(a);n.invoker=ObjHandle::of(inv);n.id=id;n.original_scale=prop_dilation.read<float>(a);g_nodes[id.token]=n;
    if(host()&&is_root){auto f=base(p::CauseKind::Root,n,*r);f.invoker=g_host.name(r->invoker);if(!vec(f_actor_pos,a,f.actor_position)||!vec(f_actor_rot,a,f.actor_rotation)){fail("root launch context unavailable");return;}f.actor_position[1]-=r->offset;emit(f);}
}
void prepare_reuse(UObject* actor){
    const auto token=ability::activation(actor);auto it=g_nodes.find(token);if(it==g_nodes.end()||!it->second.id.active)return;
    const auto id=it->second.id;
    if(!host()&&it->second.scaled){*prop_dilation.at<float>(actor)=it->second.original_scale;it->second.scaled=false;}
    end(id); // original IsActive=false proved this exact old activation has completed before reuse
}
UObject* query(const ability::Identity& id,UObject*,float min,float range,bool los,std::uint8_t channel,bool front,UObject* local){
    auto* n=node(id);auto* r=root(id);if(!n||!r||!current_view(*r))return local;
    ++n->query;r->homing=true;
    if(host()){auto f=base(p::CauseKind::Query,*n,*r);f.query=static_cast<std::uint16_t>(n->query);f.min=min;f.range=range;f.flags=(los?1:0)|(front?2:0);f.channel=channel;if(!reference(local,f,*r))return local;n->pending_query=g_out.size();n->batch=g_batch;emit(f);return local;}
    bind(*r);if(!r->wire){fail("native query has no matching Host root activation");return nullptr;}
    for(auto& e:g_records)if(!e.used&&e.fact.kind==p::CauseKind::Query&&e.fact.query==n->query&&matches(e.fact,*n,*r)){
        const auto& f=e.fact;if(std::abs(min-f.min)>.001f||std::abs(range-f.range)>.001f||f.flags!=((los?1:0)|(front?2:0))||f.channel!=channel){fail("native query parameter mismatch");return nullptr;}
        auto* a=resolve(f);
        if(f.ref_kind&&!a&&f.domain==1&&f.component){if(!scene_context::create(n->target_context,true)||!scene_context::pose(n->target_context,f.actor_position,{},V3{1,1,1},f.target_velocity)||!scene_context::aim(n->target_context,f.target_position,f.target_velocity)){fail("historical query target context unavailable");return nullptr;}a=n->target_context.actor.get();}
        if(f.ref_kind&&!a){fail("authoritative query target not ready");return nullptr;}
        if(a&&!n->target_context.actor.valid())reflect::Frame(f_actor_set).arg(0,f.actor_position).arg_bool(1,false).arg_bool(3,false).invoke(a);
        n->actor_scale=f.actor_scale;if(auto* self=n->actor.get()){*prop_dilation.at<float>(self)=g_rate*n->actor_scale;n->scaled=true;}
        e.used=true;n->pending_query=static_cast<std::size_t>(&e-g_records.data());if(a)++g_overridden;else ++g_nulls;return a;
    }
    fail("native query has no ordered Host fact");return nullptr;
}
UObject* target(UObject* component,UObject* selected){
    ability::Identity id;if(!ability::identity(component->GetOuterPrivate(),id))return selected;
    auto* n=node(id);auto* r=root(id);if(!n||!r||!current_view(*r))return selected;
    n->component=ObjHandle::of(component);n->target=ObjHandle::of(selected);n->target_actor=ObjHandle::of(selected?component_owner(selected):nullptr);
    if(g_setting_target==component)return selected;
    if(host()){
        if(n->batch==g_batch&&n->pending_query<g_out.size()&&g_out[n->pending_query].kind==p::CauseKind::Query){auto& q=g_out[n->pending_query];if(selected){p::Cause c=q;if(context(selected,c,*r)){q.component=c.component;q.target_position=c.target_position;q.target_velocity=c.target_velocity;}}}
    }else if(r->wire&&n->pending_query<g_records.size()){
        const auto& f=g_records[n->pending_query].fact;
        if(n->target_context.actor.valid()){selected=f.ref_kind&&f.component?n->target_context.aim.get():nullptr;n->target=ObjHandle::of(selected);n->target_actor=selected?n->target_context.actor:ObjHandle{};if(selected)apply_context(*n,f);return selected;}
        const auto* name=g_client.text(f.component);
        if((selected!=nullptr)!=(f.component!=0)||(selected&&(!name||relative(selected,component_owner(selected))!=*name||component_owner(selected)!=resolve(f)))){fail("original target component differs from Host");return nullptr;}
        if(selected)apply_context(*n,f);
    }return selected;
}
void homing(UObject* component,const ability::Identity& id,float delta){
    auto* n=node(id);auto* r=root(id);if(!n||!r||!r->homing||!current_view(*r))return;
    n->component=ObjHandle::of(component);
    if(host()){
        reflect::Frame q(f_get_target);auto* t=q.invoke(component)?q.ret<UObject*>():nullptr;
        auto f=base(p::CauseKind::Target,*n,*r);if(context(t,f,*r))emit(f);
    }else if(r->wire){p::Cause f;if(sample(*n,*r,f))apply_context(*n,f);else fail("native homing has no authoritative target context");}
    (void)delta;
}
void stopping(const ability::Identity& id,std::uint8_t reason){if(auto* n=node(id))n->reason=reason;}
void end(const ability::Identity& id){auto* n=node(id);auto* r=root(id);if(!n||!r||!n->id.active)return;if(host()){
    auto f=base(p::CauseKind::End,*n,*r);f.reason=n->reason;emit(f);
}else {if(auto* e=expected_end(*n,*r))e->used=true;if(n->scaled)if(auto* a=n->actor.get()){*prop_dilation.at<float>(a)=n->original_scale;n->scaled=false;}}n->id.active=false;n->target={};n->target_actor={};scene_context::destroy(n->target_context);}
std::string describe(){return std::format("  facts: roots={} nodes={} ordinals={} declarations={} records={} samples={} bound={} queryOverrides={} explicitNull={} stopDeferred={} stopAuthorized={} stopForced={} contexts={} retired={} fence={} playback={:.3f} fault='{}'\n",g_roots.size(),g_nodes.size(),g_ordinals.size(),g_declarations.size(),g_records.size(),g_samples.size(),g_bound,g_overridden,g_nulls,g_deferred,g_authorized,g_forced,g_contexts,g_retired,g_fence,g_time,g_fault)+scene_context::describe();}
}
}
