#include "coop/command_context.h"
#include "coop/native_lily.h"
#include "coop/autonomous.h"
#include "core/schema.h"
#include "core/player.h"
#include "engine/reflect.h"
#include "base/clock.h"
#include "base/hook.h"
#include "base/json.h"
#include "base/log.h"
#include "base/preflight.h"
#include <Unreal/Hooks/Hooks.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <format>
#include <unordered_map>

namespace elmp::coop::command_context {
namespace {
struct Schema {
    ObjHandle command_class, timeline_class, entry_class, queue_class;
    reflect::PropRef pawn, timeline, entries, state, action, result, command_id, frame_id, ttl, queue_pawn, queued,settings_action;
    reflect::FuncRef current;
    ObjHandle gameplay_class;
    ObjHandle summon_class;
    reflect::PropRef spirit_commands,available_commands,command_classes;
    int conditioned_stride=0;
    reflect::PropRef animations, loop, consume_mp, state_from, state_to, cached_anim, skeleton, scale;
    std::array<reflect::PropRef,4> timelines, timeline_entries;
    reflect::FuncRef invoked_direction, invoked_button, elapsed, clear, add, request_end;
    int animation_stride = 0;
    int stride = 0;
    bool live() const { return command_class.valid() && timeline_class.valid() && entry_class.valid() && queue_class.valid() && current.handle.valid(); }
} g_schema;
struct Active {
    ObjHandle pawn;
    native_lily::Source source;
    std::uint64_t token = 0;
    std::uint64_t parent = 0;
    std::uint32_t start_cursor = 0;
    std::vector<ObjHandle> states;
};
std::unordered_map<ObjHandle, Active> g_active;
struct Execution { ObjHandle pawn; native_lily::Source source; std::uint64_t token=0; };
const Execution* g_execution=nullptr;
struct ActionScope {
    Execution value;const Execution* previous=g_execution;
    explicit ActionScope(const Active& a):value{a.pawn,a.source,a.token}{value.source.command_tick=true;g_execution=&value;}
    ~ActionScope(){g_execution=previous;}
};
struct Pending { ObjHandle queue,pawn,type,action;std::int32_t id=0,frame=0;native_lily::Source source;std::uint64_t parent=0;bool queued=false; };
std::unordered_map<ObjHandle,Pending> g_pending;
std::uint64_t g_derived_queued=0,g_inherited=0,g_pending_pruned=0;
std::uint64_t g_input_queued=0,g_input_bound=0;
std::deque<json::Value> g_trace;
bool g_enabled = false, g_tracing = false;
std::string g_fault;
std::uint64_t g_next = 0, g_starts = 0, g_ends = 0, g_baselines = 0, g_expired = 0;
std::uint64_t g_restored = 0, g_restore_ends = 0;
struct RestoreAttempt { ObjHandle pawn, cls; std::uint64_t token=0; };
RestoreAttempt* g_restore_attempt=nullptr; // synchronous game-thread scope; never retained after restore
RC::Unreal::Hook::GlobalCallbackId g_post{};
bool g_post_live = false;
using Start = void(__fastcall*)(ue::UObject*);
using End = void(__fastcall*)(ue::UObject*, std::uint8_t);
Start g_start = nullptr;
End g_end = nullptr;
using Sample=std::uint8_t(__fastcall*)(ue::UObject*,float);
using Add=std::int32_t(__fastcall*)(ue::UObject*,std::uint8_t,std::uint8_t,const void*,std::int32_t,bool);
Sample g_sample=nullptr;Add g_add=nullptr;
using Admit=std::int32_t(__fastcall*)(ue::UObject*,const void*,const void*,std::int32_t);
using Accept=bool(__fastcall*)(ue::UObject*,ue::UObject*);
Admit g_admit=nullptr;Accept g_accept=nullptr;
using Create=ue::UObject*(__fastcall*)(ue::UObject*,std::uint8_t,std::uint8_t,const void*);
Create g_create=nullptr;
void prune(bool final=false);
ue::UObject* queue_of(ue::UObject* pawn){
    const auto* s=core::schema::script();if(!s||!pawn)return nullptr;
    if(pawn->IsA(static_cast<ue::UClass*>(s->zenith_character_class.get())))return s->zc_command_queue.object(pawn);
    // ZenithSpirit derives from ACharacter, not ZenithCharacter. Its component layout is different.
    reflect::Frame component(s->actor_component_by_class);
    return component.arg(0,g_schema.queue_class.get()).invoke(pawn)?component.ret<ue::UObject*>():nullptr;
}
ue::UObject* current_action(ue::UObject* pawn) {
    auto* queue=queue_of(pawn);
    reflect::Frame f(g_schema.current);
    auto* command=queue&&f.invoke(queue)?f.ret<ue::UObject*>():nullptr;
    return command&&g_schema.result.read<std::uint8_t>(command)==1?g_schema.action.object(command):nullptr;
}
bool eligible(ue::UObject* action,unsigned depth=0) {
    if(depth>=4)return false;
    auto* type=static_cast<ue::UClass*>(g_schema.gameplay_class.get());
    if(!action||!type||!action->IsA(type)||g_schema.consume_mp.read_bool(action))return false;
    const auto& animations=reflect::array_at(action,g_schema.animations);
    if(animations.num<=0||animations.num>32||!animations.data||!g_schema.loop.read_bool(animations.data+(animations.num-1)*g_schema.animation_stride))return false;
    for(unsigned i=0;i<4;++i)if(reflect::array_at(g_schema.timelines[i].at<void>(action),g_schema.timeline_entries[i]).num!=0)return false;
    const auto& states=reflect::array_at(g_schema.timeline.at<void>(action),g_schema.entries);
    if(states.num<0||states.num>128||(states.num&&!states.data))return false;
    for(int i=0;i<states.num;++i){auto* row=states.data+i*g_schema.stride;if(g_schema.state_from.read<float>(row)!=0||g_schema.state_to.read<float>(row)!=0)return false;}
    if(action->IsA(static_cast<ue::UClass*>(g_schema.summon_class.get()))){
        const auto& list=reflect::array_at(g_schema.spirit_commands.at<void>(action),g_schema.available_commands);
        if(list.num<=0||list.num>16||!list.data)return false;
        for(int i=0;i<list.num;++i){const auto& classes=reflect::array_at(list.data+i*g_schema.conditioned_stride,g_schema.command_classes);
            if(classes.num<=0||classes.num>16||!classes.data)return false;
            for(int j=0;j<classes.num;++j){auto* cls=reinterpret_cast<ue::UClass**>(classes.data)[j];ue::UObject* cdo=ue::alive(cls)?cls->GetClassDefaultObject():nullptr;if(!eligible(cdo,depth+1))return false;}
        }
    }
    return true;
}
void sample() {
    if(!g_enabled)return;prune(true);if(!g_tracing)return;
    const auto* s=core::schema::script();if(!s)return;
    for(const auto& [weak,a]:g_active)if(auto* command=weak.get();command&&a.pawn.valid()) {
        auto* anim=g_schema.cached_anim.object(command);auto* skeleton=anim?g_schema.skeleton.object(anim):nullptr;
        reflect::Frame current(s->sp_get_current);if(!skeleton||!current.arg(0,0).invoke(skeleton))continue;
        auto* track=current.ret<ue::UObject*>();if(!track)continue;
        reflect::Frame name(s->te_animation_name),time(s->te_track_time);
        if(!name.invoke(track)||!time.invoke(track))continue;
        auto j=json::Value::object();j.set("type","sample");j.set("t_us",clock::now_us());j.set("token",static_cast<std::int64_t>(a.token));
        j.set("seat",a.source.seat);j.set("input",static_cast<std::int64_t>(a.source.seq));j.set("generation",static_cast<std::int64_t>(a.source.generation));
        j.set("command",log::utf8(command->GetClassPrivate()->GetPathName()));j.set("pawn",log::utf8(a.pawn.get()->GetFullName()));
        native_lily::Source source;if(native_lily::source(a.pawn.get(),source))j.set("cursor",static_cast<std::int64_t>(source.seq));
        j.set("anim",log::utf8(name.ret_string()));j.set("time",time.ret<float>());j.set("scale",g_schema.scale.read<float>(a.pawn.get()));
        if(auto at=core::location(a.pawn.get())){j.set("y",at->y);j.set("z",at->z);}
        if(g_trace.size()==1024)g_trace.pop_front();g_trace.push_back(std::move(j));
    }
}
void fail(std::string why) { if (g_fault.empty()) g_fault = std::move(why); }
void pipeline(std::string_view event,ue::UObject* queue,ue::UObject* cmd,int accepted=-1) {
    if(!g_tracing||!cmd)return;
    auto* pawn=g_schema.queue_pawn.object(queue);native_lily::Source src;
    if(!pawn||!native_lily::source(pawn,src)||src.domain)return;
    auto* action=g_schema.action.object(cmd);if(!action)return;
    auto row=json::Value::object();row.set("type",std::string(event));row.set("t_us",clock::now_us());
    row.set("seat",src.seat);row.set("input",static_cast<std::int64_t>(src.seq));
    row.set("id",g_schema.command_id.read<std::int32_t>(cmd));row.set("frame",g_schema.frame_id.read<std::int32_t>(cmd));
    row.set("ttl",g_schema.ttl.read<float>(cmd));row.set("accepted",accepted);row.set("command",log::utf8(action->GetClassPrivate()->GetPathName()));
    const auto list=reflect::array_at(queue,g_schema.queued);
    if(list.num>0&&list.num<=4096&&list.data)if(auto* head=reinterpret_cast<ue::UObject**>(list.data)[0])if(auto* running=g_schema.action.object(head)){
        row.set("head",log::utf8(running->GetClassPrivate()->GetPathName()));reflect::Frame f(g_schema.elapsed);if(f.invoke(running))row.set("elapsed",f.ret<float>());
    }
    if(g_trace.size()==1024)g_trace.pop_front();g_trace.push_back(std::move(row));
}
ue::UObject* queued_id(ue::UObject* queue,std::int32_t id){
    const auto list=reflect::array_at(queue,g_schema.queued);if(id<0||list.num<0||list.num>4096||(list.num&&!list.data))return nullptr;
    for(int i=list.num-1;i>=0;--i){auto* cmd=reinterpret_cast<ue::UObject**>(list.data)[i];if(ue::alive(cmd)&&g_schema.command_id.read<std::int32_t>(cmd)==id)return cmd;}return nullptr;
}
std::int32_t __fastcall admit(ue::UObject* queue,const void* input,const void* settings,std::int32_t frame){
    const auto id=g_admit(queue,input,settings,frame);
    if(g_enabled&&ue::on_game_thread()){
        if(g_tracing)pipeline("admit.input",queue,queued_id(queue,id));
    }
    return id;
}
bool __fastcall accept(ue::UObject* queue,ue::UObject* cmd){
    const auto allowed=g_accept(queue,cmd);
    if(g_enabled&&g_tracing&&ue::on_game_thread())pipeline("accept",queue,cmd,allowed);
    return allowed;
}
void prune(bool final) {
    std::erase_if(g_active, [](const auto& e) {
        if (e.first.valid() && e.second.pawn.valid()) return false;
        ++g_expired; return true;
    });
    std::erase_if(g_pending,[final](auto& e){
        auto* cmd=e.first.get();auto* queue=e.second.queue.get();
        auto& v=e.second;
        bool keep=cmd&&queue&&v.pawn.valid()&&v.type.valid()&&g_schema.command_id.read<std::int32_t>(cmd)==v.id;
        if(keep){
            const auto ar=reflect::array_at(queue,g_schema.queued);keep=ar.num>=0&&ar.num<=4096&&(!ar.num||ar.data);
            if(keep){
                auto** p=reinterpret_cast<ue::UObject**>(ar.data);const bool present=ar.num&&std::find(p,p+ar.num,cmd)!=p+ar.num;
                if(!present)keep=!v.queued&&!final;
                else {
                    auto* action=g_schema.action.object(cmd);keep=action&&action->GetClassPrivate()==v.type.get();
                    if(keep&&!v.queued){v.action=ObjHandle::of(action);v.frame=g_schema.frame_id.read<std::int32_t>(cmd);v.queued=true;if(v.parent)++g_derived_queued;else ++g_input_queued;}
                    if(keep)keep=v.action.get()==action&&v.frame==g_schema.frame_id.read<std::int32_t>(cmd);
                }
            }
        }
        if(!keep)++g_pending_pruned;return !keep;
    });
}
void record(std::string_view event, ue::UObject* command, const Active& a, int reason = -1) {
    if (!g_tracing) return;
    auto j = json::Value::object();
    j.set("type", std::string(event)); j.set("t_us", clock::now_us());
    j.set("token", static_cast<std::int64_t>(a.token));
    j.set("parent",static_cast<std::int64_t>(a.parent));
    j.set("start_cursor",static_cast<std::int64_t>(a.start_cursor));
    j.set("seat", a.source.seat); j.set("input", static_cast<std::int64_t>(a.source.seq));
    j.set("generation", static_cast<std::int64_t>(a.source.generation));
    j.set("domain", a.source.domain);
    j.set("pawn", a.pawn.valid() ? log::utf8(a.pawn.get()->GetFullName()) : "expired");
    j.set("command", ue::alive(command) ? log::utf8(command->GetClassPrivate()->GetPathName()) : "expired");
    j.set("reason", reason);
    if (g_trace.size() == 1024) g_trace.pop_front();
    g_trace.push_back(std::move(j));
}
void collect(ue::UObject* command, Active& a) {
    const auto& arr = reflect::array_at(g_schema.timeline.at<void>(command), g_schema.entries);
    if (arr.num < 0 || arr.num > 128 || (arr.num && !arr.data)) return fail("command state timeline layout/capacity");
    a.states.clear();
    for (int i = 0; i < arr.num; ++i) {
        auto* state = g_schema.state.object(arr.data + i * g_schema.stride);
        if (!state) continue;
        const auto h = ObjHandle::of(state);
        if (std::find(a.states.begin(), a.states.end(), h) == a.states.end()) a.states.push_back(h);
    }
}
bool insert(ue::UObject* command, Active& a) {
    auto* pawn = g_schema.pawn.object(command);
    native_lily::Source source;
    if (!pawn || !native_lily::source(pawn, source)) return false;
    prune();
    if (g_active.size() >= 512 && !g_active.contains(ObjHandle::of(command))) { fail("active command ownership capacity"); return false; }
    a.pawn = ObjHandle::of(pawn); a.source = source; a.token = ++g_next;a.start_cursor=source.seq;
    auto* queue=queue_of(pawn);
    // Native Start runs before UCommand sets IN_PROGRESS. GetCurrentCommand intentionally
    // returns null at that point; the native processor has already moved its command to head.
    const auto ar=queue?reflect::array_at(queue,g_schema.queued):reflect::RawArray{};
    if(ar.num<0||ar.num>4096||(ar.num&&!ar.data)){fail("command startup queue layout");return false;}
    auto* cmd=ar.num?reinterpret_cast<ue::UObject**>(ar.data)[0]:nullptr;
    if(cmd&&g_schema.action.object(cmd)==command)if(auto p=g_pending.find(ObjHandle::of(cmd));p!=g_pending.end()){
        const auto& v=p->second;
        if(v.queued&&v.queue.get()==queue&&v.pawn==a.pawn&&v.action.get()==command&&v.id==g_schema.command_id.read<std::int32_t>(cmd)&&v.frame==g_schema.frame_id.read<std::int32_t>(cmd)&&v.source.seat==source.seat&&v.source.generation==source.generation&&v.source.domain==source.domain){a.source=v.source;a.parent=v.parent;if(v.parent)++g_inherited;else ++g_input_bound;}
        g_pending.erase(p);
    }
    collect(command, a);
    g_active[ObjHandle::of(command)] = a;
    return true;
}
void __fastcall start(ue::UObject* command) {
    if (!g_enabled || !ue::on_game_thread()) return g_start(command);
    autonomous::command_started(command, g_schema.pawn.object(command));
    Active a;
    const bool tracked = insert(command, a);
    if(tracked&&g_restore_attempt&&g_restore_attempt->pawn==a.pawn&&g_restore_attempt->cls.get()==command->GetClassPrivate())
        g_restore_attempt->token=a.token;
    ActionScope scope(a);
    g_start(command);
    if (tracked) {
        const auto i = g_active.find(ObjHandle::of(command));
        if (i != g_active.end() && i->second.token == a.token) collect(command, i->second);
        ++g_starts; record("start", command, a);
    }
}
void __fastcall end(ue::UObject* command, std::uint8_t reason) {
    if (!g_enabled || !ue::on_game_thread()) return g_end(command, reason);
    const auto h = ObjHandle::of(command);
    const auto i = g_active.find(h);
    const bool tracked = i != g_active.end();
    const auto before = tracked ? i->second : Active{};
    autonomous::command_ended(command, g_schema.pawn.object(command), reason);
    ActionScope scope(before);
    g_end(command, reason);
    if (tracked) {
        ++g_ends; record("end", command, before, reason);
        const auto after = g_active.find(h);
        if (after != g_active.end() && after->second.token == before.token) g_active.erase(after);
    }
}
std::uint8_t __fastcall sample_command(ue::UObject* command,float dt){
    if(!g_enabled||!ue::on_game_thread())return g_sample(command,dt);
    const auto i=g_active.find(ObjHandle::of(command));if(i==g_active.end())return g_sample(command,dt);
    ActionScope scope(i->second);return g_sample(command,dt);
}
std::int32_t __fastcall add_command(ue::UObject* queue,std::uint8_t direction,std::uint8_t button,const void* settings,std::int32_t frame,bool front){
    const auto id=g_add(queue,direction,button,settings,frame,front);
    if(!g_enabled||!ue::on_game_thread()||id<0)return id;
    if(g_tracing)pipeline("admit.explicit",queue,queued_id(queue,id));
    return id;
}
ue::UObject* __fastcall create(ue::UObject* queue,std::uint8_t direction,std::uint8_t button,const void* settings){
    if(!g_enabled||!ue::on_game_thread())return g_create(queue,direction,button,settings);
    auto* pawn=g_schema.queue_pawn.object(queue);native_lily::Source src;
    const bool tracked=pawn&&native_lily::source(pawn,src)&&src.seq&&!src.domain;
    const auto owner=ObjHandle::of(pawn),type=ObjHandle::of(g_schema.settings_action.object(settings));
    const auto parent=g_execution&&g_execution->token&&g_execution->pawn.get()==pawn?g_execution->token:0;
    if(parent)src=g_execution->source;
    auto* cmd=g_create(queue,direction,button,settings);if(!cmd)return cmd;
    const auto h=ObjHandle::of(cmd);g_pending.erase(h);
    if(!tracked||!owner.valid()||!type.valid())return cmd;
    prune();if(g_pending.size()>=512){fail("admitted command source capacity");return cmd;}
    g_pending[h]={ObjHandle::of(queue),owner,type,{},g_schema.command_id.read<std::int32_t>(cmd),0,src,parent,false};
    return cmd;
}
bool install() {
    auto* cls = reflect::find_class(L"/Script/Zenith.CommandAction");
    reflect::Resolver c("command context", cls);
    g_schema.pawn = c.prop(L"Pawn", reflect::Kind::Object);
    g_schema.timeline = c.prop(L"StateTimeline", reflect::Kind::Struct);
    g_schema.consume_mp = c.prop(L"bConsumeMP", reflect::Kind::Bool);
    g_schema.cached_anim = c.prop(L"CachedAnimComponent",reflect::Kind::Object);
    g_schema.invoked_direction=c.func(L"GetInvokedByDirectionInputType",0);g_schema.invoked_button=c.func(L"GetInvokedByInputType",0);g_schema.elapsed=c.func(L"GetExecutionTime",0);
    g_schema.request_end=c.func(L"RequestEnd",1);
    const auto start_fn = c.func(L"StartCommandAction", 0), end_fn = c.func(L"EndCommandAction", 1), sample_fn=c.func(L"SampleCommandAction",1);
    auto* tl = static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.StateTimeline"));
    auto* en = static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.StateTimelineEntry"));
    reflect::Resolver t("command states", tl), e("command state entry", en);
    g_schema.entries = t.prop(L"StateEntries", reflect::Kind::Array);
    g_schema.state = e.prop(L"State", reflect::Kind::Class);
    g_schema.state_from=e.prop(L"From",reflect::Kind::Float,4);g_schema.state_to=e.prop(L"To",reflect::Kind::Float,4);
    auto* queue_class=reflect::find_class(L"/Script/Zenith.CommandQueueComponent");g_schema.queue_class=ObjHandle::of(queue_class);
    reflect::Resolver q("command queue baseline", queue_class);
    g_schema.current = q.func(L"GetCurrentCommand", 0);
    g_schema.clear=q.func(L"ClearCommandBuffer",0);g_schema.add=q.func(L"AddNewImmediateCommand",3);
    const auto add_fn=q.func(L"AddNewCommand",5);g_schema.queue_pawn=q.prop(L"Pawn",reflect::Kind::Object);g_schema.queued=q.prop(L"CommandQueue",reflect::Kind::Array);
    reflect::Resolver cmd("current command", reflect::find_class(L"/Script/Zenith.Command"));
    g_schema.action = cmd.prop(L"CommandAction", reflect::Kind::Object);
    g_schema.result = cmd.prop(L"CurrentResult", reflect::Kind::Any, 1);
    g_schema.command_id=cmd.prop(L"CommandID",reflect::Kind::Int,4);
    g_schema.frame_id=cmd.prop(L"FrameID",reflect::Kind::Int,4);g_schema.ttl=cmd.prop(L"TTL",reflect::Kind::Float,4);
    reflect::Resolver settings("admitted settings",static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.CommandSettings")));
    g_schema.settings_action=settings.prop(L"CommandActionType",reflect::Kind::Class);
    if(!settings.ok())return false;
    if (!c.ok() || !t.ok() || !e.ok() || !q.ok() || !cmd.ok()) { fail(c.failure()+t.failure()+e.failure()+q.failure()+cmd.failure()); return false; }
    auto* gameplay=reflect::find_class(L"/Script/Zenith.CommandActionGameplay");reflect::Resolver gp("persistent command",gameplay);
    g_schema.animations=gp.prop(L"Animations",reflect::Kind::Array);g_schema.gameplay_class=ObjHandle::of(gameplay);
    reflect::Resolver an("persistent animation",static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.SpineAnimationDefinition")));
    g_schema.loop=an.prop(L"bLoop",reflect::Kind::Bool);g_schema.animation_stride=reflect::element_size(g_schema.animations);
    auto* summon=reflect::find_class(L"/Script/Zenith.CommandActionSummon");reflect::Resolver su("persistent summon",summon);
    g_schema.spirit_commands=su.prop(L"SpiritCommandList",reflect::Kind::Struct);g_schema.summon_class=ObjHandle::of(summon);
    reflect::Resolver cl("persistent command list",static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.ConditionedCommandList")));
    g_schema.available_commands=cl.prop(L"AvailableCommands",reflect::Kind::Array);g_schema.conditioned_stride=reflect::element_size(g_schema.available_commands);
    reflect::Resolver ce("persistent conditioned command",static_cast<ue::UStruct*>(reflect::find_object(L"/Script/Zenith.ConditionedCommand")));
    g_schema.command_classes=ce.prop(L"CommandActionClasses",reflect::Kind::Array);
    if(!su.ok()||!cl.ok()||!ce.ok()||g_schema.conditioned_stride<=0||reflect::element_size(g_schema.command_classes)!=8){fail(su.failure()+cl.failure()+ce.failure());return false;}
    const std::array<std::wstring_view,4> names{L"AbilityTimeline",L"MoveTimeline",L"FXTimeline",L"SoundTimeline"};
    const std::array<std::wstring_view,4> fields{L"Entries",L"MoveEntries",L"FXEntries",L"SoundEntries"};
    for(unsigned i=0;i<4;++i){g_schema.timelines[i]=c.prop(names[i],reflect::Kind::Struct);reflect::Resolver timeline("persistent empty timeline",static_cast<ue::UStruct*>(reflect::find_object(std::wstring(L"/Script/Zenith.")+std::wstring(names[i]))));g_schema.timeline_entries[i]=timeline.prop(fields[i],reflect::Kind::Array);if(!timeline.ok()){fail(timeline.failure());return false;}}
    reflect::Resolver sa("command animation",reflect::find_class(L"/Script/Zenith.SpineAnimationComponent"));g_schema.skeleton=sa.prop(L"AnimComponent",reflect::Kind::Object);
    reflect::Resolver actor("command actor clock",reflect::find_class(L"/Script/Engine.Actor"));g_schema.scale=actor.prop(L"CustomTimeDilation",reflect::Kind::Float,4);
    if(!c.ok()||!gp.ok()||!an.ok()||!sa.ok()||!actor.ok()||g_schema.animation_stride<=0){fail(c.failure()+gp.failure()+an.failure()+sa.failure()+actor.failure());return false;}
    g_schema.stride = reflect::element_size(g_schema.entries);
    if (g_schema.stride <= 0 || g_schema.state.offset + g_schema.state.size > g_schema.stride) return false;
    auto* sx = reinterpret_cast<const std::uint8_t*>(start_fn.fn->GetFuncPtr());
    auto* ex = reinterpret_cast<const std::uint8_t*>(end_fn.fn->GetFuncPtr());
    if (sx != preflight::address("CommandStartExec") || ex != preflight::address("CommandEndExec") || sx[25] != 0xFF || sx[26] != 0xA0 || ex[0x6B] != 0xE8) return false;
    std::uint32_t slot = 0; std::memcpy(&slot, sx + 27, 4);
    ue::UObject* cdo = cls->GetClassDefaultObject();
    if (!cdo || slot != 624) return false;
    void* start_target = *reinterpret_cast<void**>(*reinterpret_cast<std::uint8_t**>(cdo) + slot);
    std::int32_t relative = 0; std::memcpy(&relative, ex + 0x6C, 4);
    auto* end_target = const_cast<std::uint8_t*>(ex + 0x70 + relative);
    if (start_target != preflight::address("CommandStartNative") || end_target != preflight::address("CommandEndNative")) return false;
    if (!g_start && !hook::install_address("CommandStartNative", start_target, reinterpret_cast<void*>(&start), reinterpret_cast<void**>(&g_start), "reflect")) return false;
    if (!g_end && !hook::install_address("CommandEndNative", end_target, reinterpret_cast<void*>(&end), reinterpret_cast<void**>(&g_end), "reflect")) return false;
    const auto* sm=reinterpret_cast<const std::uint8_t*>(sample_fn.fn->GetFuncPtr());
    const auto* ad=reinterpret_cast<const std::uint8_t*>(add_fn.fn->GetFuncPtr());
    if(!sm||!ad||sm[0x7B]!=0xFF||sm[0x7C]!=0x90||ad[0x1EF]!=0xE8)return false;
    std::uint32_t sample_slot=0;std::memcpy(&sample_slot,sm+0x7D,4);
    std::int32_t add_relative=0;std::memcpy(&add_relative,ad+0x1F0,4);
    auto* sample_target=sample_slot==632?*reinterpret_cast<void**>(*reinterpret_cast<std::uint8_t**>(cdo)+sample_slot):nullptr;
    auto* add_target=const_cast<std::uint8_t*>(ad+0x1F4+add_relative);
    if(sample_target!=preflight::address("CommandSampleNative")||add_target!=preflight::address("CommandQueueAddNative")||reflect::element_size(g_schema.queued)!=8)return false;
    if(add_target[0x1F]!=0xE8)return false;
    std::int32_t cr=0;std::memcpy(&cr,add_target+0x20,4);auto* ct=add_target+0x24+cr;
    if(ct!=preflight::address("CommandCreateNative"))return false;
    if(!g_create&&!hook::install_address("CommandCreateNative",ct,reinterpret_cast<void*>(&create),reinterpret_cast<void**>(&g_create),"reflect"))return false;
    if(!g_sample&&!hook::install_address("CommandSampleNative",sample_target,reinterpret_cast<void*>(&sample_command),reinterpret_cast<void**>(&g_sample),"reflect"))return false;
    if(!g_add&&!hook::install_address("CommandQueueAddNative",add_target,reinterpret_cast<void*>(&add_command),reinterpret_cast<void**>(&g_add),"reflect"))return false;
    const auto admit_fn=q.func(L"TryAddCommandsFromSettings",3);
    const auto* ax=admit_fn.ok()?reinterpret_cast<const std::uint8_t*>(admit_fn.fn->GetFuncPtr()):nullptr;
    if(!ax||ax[0x1B8]!=0xE8)return false;
    std::int32_t ar=0;std::memcpy(&ar,ax+0x1B9,4);auto* at=const_cast<std::uint8_t*>(ax+0x1BD+ar);
    if(at!=preflight::address("CommandInputAdmissionNative"))return false;
    if(!g_admit&&!hook::install_address("CommandInputAdmissionNative",at,reinterpret_cast<void*>(&admit),reinterpret_cast<void**>(&g_admit),"reflect"))return false;
    if(!g_accept&&!hook::install_manifest("CommandAcceptNative",reinterpret_cast<void*>(&accept),reinterpret_cast<void**>(&g_accept)))return false;
    g_schema.command_class = ObjHandle::of(cls); g_schema.timeline_class = ObjHandle::of(tl); g_schema.entry_class = ObjHandle::of(en);
    if(!g_post_live){RC::Unreal::Hook::FCallbackOptions o;o.OwnerModName=STR("ELMP");o.HookName=STR("coop.command_context.trace");g_post=RC::Unreal::Hook::RegisterEngineTickPostCallback([](auto&,RC::Unreal::UEngine*,float,bool){sample();},o);g_post_live=true;}
    return true;
}
}
bool enable() { if (g_enabled && g_schema.live()) return true; return g_enabled = install(); }
void reset() { g_enabled = false;if(g_post_live){RC::Unreal::Hook::UnregisterCallback(g_post);g_post_live=false;}g_active.clear();g_pending.clear(); g_schema = {}; g_fault.clear(); }
void forget(ue::UObject* pawn) { const auto h=ObjHandle::of(pawn); std::erase_if(g_active,[&](const auto& e){return e.second.pawn==h;});std::erase_if(g_pending,[&](const auto& e){return e.second.pawn==h;}); }
bool source(ue::UObject* pawn,native_lily::Source& out){if(!g_enabled||!g_execution||!g_execution->token||g_execution->pawn.get()!=pawn)return false;out=g_execution->source;return out.seq!=0;}
bool retained_source(const native_lily::Source& source){
    const auto matches=[&](const auto& s){return s.seat==source.seat&&s.generation==source.generation&&s.seq==source.seq&&s.domain==source.domain;};
    return std::any_of(g_active.begin(),g_active.end(),[&](const auto& e){return e.first.valid()&&e.second.pawn.valid()&&matches(e.second.source);})||std::any_of(g_pending.begin(),g_pending.end(),[&](const auto& e){return e.first.valid()&&e.second.pawn.valid()&&matches(e.second.source);});
}
std::vector<ue::UObject*> owned_states(ue::UObject* pawn) {
    std::vector<ue::UObject*> out;
    if (!g_enabled || !pawn || !g_schema.live()) return out;
    prune();
    // A host may begin hosting while a command is already running. Do not invent a Start event.
    const auto* s = core::schema::script();
    reflect::Frame current(g_schema.current);
    auto* queue = queue_of(pawn);
    if (queue && current.invoke(queue)) if (auto* cmd=current.ret<ue::UObject*>();cmd && g_schema.result.read<std::uint8_t>(cmd)==1) {
        auto* action=g_schema.action.object(cmd);
        if(action && !g_active.contains(ObjHandle::of(action))) { Active a;if(insert(action,a)){++g_baselines;record("baseline",action,a);} }
    }
    const auto h = ObjHandle::of(pawn);
    for (const auto& [_,a] : g_active) if (a.pawn == h) for (const auto& state : a.states) if (auto* cls=state.get(); cls && std::find(out.begin(),out.end(),cls)==out.end()) out.push_back(cls);
    return out;
}
std::string fault() { return g_fault; }
Persistent persistent(ue::UObject* pawn) {
    Persistent p;auto* action=current_action(pawn);if(!eligible(action))return p;
    const auto it=g_active.find(ObjHandle::of(action));if(it==g_active.end())return p;
    reflect::Frame direction(g_schema.invoked_direction),button(g_schema.invoked_button),elapsed(g_schema.elapsed);
    if(!direction.invoke(action)||!button.invoke(action)||!elapsed.invoke(action))return p;
    p.cls=action->GetClassPrivate();p.token=it->second.token;p.input=it->second.source.seq;p.direction=direction.ret<std::uint8_t>();p.button=button.ret<std::uint8_t>();p.elapsed=elapsed.ret<float>();return p;
}
std::uint64_t restore(ue::UObject* pawn,ue::UObject* type,std::uint8_t direction,std::uint8_t button) {
    auto* cls=static_cast<ue::UClass*>(type);ue::UObject* cdo=cls?cls->GetClassDefaultObject():nullptr;
    if(!eligible(cdo)){fail("Host persistent command is not restorable from its native data");return 0;}
    auto* queue=queue_of(pawn);
    if(!queue||!reflect::Frame(g_schema.clear).invoke(queue)){fail("persistent command queue unavailable");return 0;}
    RestoreAttempt attempt{ObjHandle::of(pawn),ObjHandle::of(type)};
    struct Scope {RestoreAttempt* previous=g_restore_attempt;Scope(RestoreAttempt& a){g_restore_attempt=&a;}~Scope(){g_restore_attempt=previous;}} scope(attempt);
    const auto before_starts=g_starts,before_ends=g_ends;
    reflect::Frame add(g_schema.add);if(!add.arg(0,direction).arg(1,button).arg(2,type).invoke(queue)||add.ret<std::int32_t>()<0){fail("persistent command could not enter native queue");return 0;}
    if(!attempt.token){
        reflect::Frame current(g_schema.current);auto* cmd=current.invoke(queue)?current.ret<ue::UObject*>():nullptr;
        auto* action=cmd?g_schema.action.object(cmd):nullptr;
        fail(std::format("native persistent command did not start: requested={} pawn={} id={} current={} result={} starts={} ends={}",log::utf8(type->GetPathName()),log::utf8(pawn->GetFullName()),add.ret<std::int32_t>(),action?log::utf8(action->GetClassPrivate()->GetPathName()):"<none>",cmd?g_schema.result.read<std::uint8_t>(cmd):0,g_starts-before_starts,g_ends-before_ends));return 0;
    }
    ++g_restored;return attempt.token; // the original zero-delta update may already have ended it
}
void end_restored(ue::UObject* pawn,std::uint64_t token) {
    auto* command=current_action(pawn);auto it=g_active.find(ObjHandle::of(command));
    if(command&&it!=g_active.end()&&it->second.token==token&&eligible(command)){if(reflect::Frame(g_schema.request_end).arg_bool(0,false).invoke(command))++g_restore_ends;else fail("persistent command could not request native end");}
}
std::string describe() { prune();return std::format("command context: enabled={} active={} starts={} ends={} baselines={} expired={} restored={} restoreEnds={} trace={} fault='{}'\n  derived queued={} inherited={} pending={} pruned={}\n  input admitted={} started={}\n",g_enabled,g_active.size(),g_starts,g_ends,g_baselines,g_expired,g_restored,g_restore_ends,g_trace.size(),g_fault,g_derived_queued,g_inherited,g_pending.size(),g_pending_pruned,g_input_queued,g_input_bound); }
std::string trace(std::string_view action) {
    if(action=="on"){g_trace.clear();g_tracing=true;return "command trace enabled\n";}
    if(action=="off"){g_tracing=false;return "command trace disabled\n";}
    auto a=json::Value::array();for(const auto& row:g_trace)a.push(row);return json::dump(a,0)+"\n";
}
}
