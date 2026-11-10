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
#include <intrin.h>
#include <array>
#include <cstring>
#include <deque>
#include <format>
#include <unordered_map>

namespace elmp::coop::command_context {
namespace {
struct Schema {
    ObjHandle command_class, timeline_class, entry_class, queue_class;
    reflect::PropRef pawn, timeline, entries, state, action, result, command_id, queue_pawn, queued;
    reflect::FuncRef current;
    ObjHandle climb_class;
    reflect::PropRef climb_owner;
    ObjHandle gameplay_class;
    ObjHandle summon_class;
    reflect::PropRef spirit_commands,available_commands,command_classes;
    int conditioned_stride=0;
    reflect::PropRef animations, loop, consume_mp, state_from, state_to, cached_anim, skeleton, scale;
    std::array<reflect::PropRef,4> timelines, timeline_entries;
    reflect::FuncRef invoked_direction, invoked_button, clear, add, request_end;
    int animation_stride = 0;
    int stride = 0;
    bool live() const { return command_class.valid() && timeline_class.valid() && entry_class.valid() && queue_class.valid() && current.handle.valid(); }
} g_schema;
struct Active {
    ObjHandle pawn;
    native_lily::Source source;
    std::uint64_t token = 0;
    std::uint64_t parent = 0;
    bool queue_owner = false;
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
struct Pending { ObjHandle queue,pawn,action;std::int32_t id=0;native_lily::Source source;std::uint64_t parent=0; };
std::unordered_map<ObjHandle,Pending> g_pending;
std::uint64_t g_derived_queued=0,g_inherited=0,g_pending_pruned=0;
std::deque<json::Value> g_trace;
bool g_enabled = false, g_tracing = false;
std::string g_fault;
std::uint64_t g_next = 0, g_starts = 0, g_ends = 0, g_baselines = 0, g_expired = 0;
std::uint64_t g_restored = 0, g_climb_reads = 0;
bool g_playback_gap = false;
using Target = std::array<float, 3>;
struct RestoreAttempt {
    ObjHandle pawn, cls;
    std::uint64_t token = 0;
    ObjHandle candidate;
    const Target* target = nullptr;
    bool target_read = false;
};
struct StartRead { ue::UObject* pawn; std::uint64_t token; RestoreAttempt* replay; };
StartRead* g_start_read = nullptr;
using ReadTarget = Target*(__fastcall*)(ue::UObject*, Target*);
ReadTarget g_read_target = nullptr;
using MatchInput = std::int32_t(__fastcall*)(ue::UObject*, const void*, ue::UObject*);
MatchInput g_match_input = nullptr;
const void* g_character_match_return = nullptr;
RestoreAttempt* g_restore_attempt=nullptr; // synchronous game-thread scope; never retained after restore
RC::Unreal::Hook::GlobalCallbackId g_post{};
bool g_post_live = false;
using Start = void(__fastcall*)(ue::UObject*);
using End = void(__fastcall*)(ue::UObject*, std::uint8_t);
using Remove = void(__fastcall*)(ue::UObject*, std::int32_t, std::uint8_t);
Remove g_remove = nullptr;
using Accept = bool(__fastcall*)(ue::UObject*, ue::UObject*);
Accept g_accept = nullptr;
Start g_start = nullptr;
End g_end = nullptr;
using Sample=std::uint8_t(__fastcall*)(ue::UObject*,float);
using Add=std::int32_t(__fastcall*)(ue::UObject*,std::uint8_t,std::uint8_t,const void*,std::int32_t,bool);
Sample g_sample=nullptr;Add g_add=nullptr;
void prune();
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
    if(!g_enabled)return;prune();if(!g_tracing)return;
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
void prune() {
    std::erase_if(g_active, [](const auto& e) {
        if (e.first.valid() && e.second.pawn.valid()) return false;
        ++g_expired; return true;
    });
    std::erase_if(g_pending,[](const auto& e){
        auto* cmd=e.first.get();auto* queue=e.second.queue.get();
        bool keep=cmd&&queue&&e.second.pawn.valid()&&e.second.action.valid()&&g_schema.command_id.read<std::int32_t>(cmd)==e.second.id&&g_schema.action.object(cmd)==e.second.action.get();
        if(keep){const auto ar=reflect::array_at(queue,g_schema.queued);keep=ar.num>=0&&ar.num<=4096&&(!ar.num||ar.data);if(keep){auto** p=reinterpret_cast<ue::UObject**>(ar.data);keep=ar.num&&std::find(p,p+ar.num,cmd)!=p+ar.num;}}
        if(!keep)++g_pending_pruned;return !keep;
    });
}
void record(std::string_view event, ue::UObject* command, const Active& a, int reason = -1) {
    if (!g_tracing) return;
    auto j = json::Value::object();
    j.set("type", std::string(event)); j.set("t_us", clock::now_us());
    j.set("token", static_cast<std::int64_t>(a.token));
    j.set("parent",static_cast<std::int64_t>(a.parent));
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
    a.pawn = ObjHandle::of(pawn); a.source = source; a.token = ++g_next;
    auto* queue=queue_of(pawn);
    // Native Start runs before UCommand sets IN_PROGRESS. GetCurrentCommand intentionally
    // returns null at that point; the native processor has already moved its command to head.
    const auto ar=queue?reflect::array_at(queue,g_schema.queued):reflect::RawArray{};
    if(ar.num<0||ar.num>4096||(ar.num&&!ar.data)){fail("command startup queue layout");return false;}
    auto* cmd=ar.num?reinterpret_cast<ue::UObject**>(ar.data)[0]:nullptr;
    a.queue_owner = cmd && g_schema.action.object(cmd) == command;
    if(a.queue_owner)if(auto p=g_pending.find(ObjHandle::of(cmd));p!=g_pending.end()){
        const auto& v=p->second;
        if(v.queue.get()==queue&&v.pawn==a.pawn&&v.action.get()==command&&v.id==g_schema.command_id.read<std::int32_t>(cmd)&&v.source.seat==source.seat&&v.source.generation==source.generation&&v.source.domain==source.domain){a.source=v.source;a.parent=v.parent;++g_inherited;}
        g_pending.erase(p);
    }
    collect(command, a);
    g_active[ObjHandle::of(command)] = a;
    return true;
}
void capture_edge(ue::UObject* command, const Active& active, std::uint8_t end) {
    if (!active.queue_owner || !active.token || active.source.domain || !active.source.seq || native_lily::actor(active.source.seat)) return;
    reflect::Frame direction(g_schema.invoked_direction), button(g_schema.invoked_button);
    if (direction.invoke(command) && button.invoke(command))
        native_lily::command_edge(active.pawn.get(), active.source, command->GetClassPrivate(), active.token,
                                  direction.ret<std::uint8_t>(), button.ret<std::uint8_t>(), end);
}
bool __fastcall accept(ue::UObject* queue, ue::UObject* candidate) {
    if (!g_enabled || !ue::on_game_thread()) return g_accept(queue, candidate);
    auto* pawn = g_schema.queue_pawn.object(queue);
    if (!native_lily::remote_pawn(pawn)) return g_accept(queue, candidate);
    auto* action = g_schema.action.object(candidate);
    if (!g_restore_attempt || g_restore_attempt->token || g_restore_attempt->candidate.get() != candidate || g_restore_attempt->pawn.get() != pawn ||
        !action || g_restore_attempt->cls.get() != action->GetClassPrivate()) return false;
    g_accept(queue, candidate); // native proxy preparation at the Host-selected Start
    return true;
}
Target* __fastcall read_target(ue::UObject* component, Target* out) {
    if (!ue::on_game_thread() || !g_start_read || !component ||
        !component->IsA(static_cast<ue::UClass*>(g_schema.climb_class.get())) ||
        g_schema.climb_owner.object(component) != g_start_read->pawn)
        return g_read_target(component, out);
    if (auto* replay = g_start_read->replay) {
        if (replay->target) {
            replay->target_read = true;
            ++g_climb_reads;
            *out = *replay->target;
            return out;
        }
        if (g_fault.empty()) g_playback_gap = true;
        fail("remote command read a climb target absent from the Host Start");
        // Finish the synchronous native call under the client settlement gates.
        return g_read_target(component, out);
    }
    auto* result = g_read_target(component, out);
    native_lily::capture_climb_target(g_start_read->pawn, g_start_read->token, *result);
    return result;
}
std::int32_t __fastcall match_input(ue::UObject* queue, const void* snapshot, ue::UObject* set) {
    if (_ReturnAddress() == g_character_match_return && g_enabled && ue::on_game_thread() &&
        native_lily::remote_pawn(g_schema.queue_pawn.object(queue))) return -1;
    return g_match_input(queue, snapshot, set);
}
void __fastcall start(ue::UObject* command) {
    if (!g_enabled || !ue::on_game_thread()) return g_start(command);
    autonomous::command_started(command, g_schema.pawn.object(command));
    Active a;
    const bool tracked = insert(command, a);
    if(tracked && a.queue_owner && g_restore_attempt && !g_restore_attempt->token && g_restore_attempt->pawn==a.pawn && g_restore_attempt->cls.get()==command->GetClassPrivate())
        g_restore_attempt->token=a.token;
    if (tracked) capture_edge(command, a, 0);
    ActionScope scope(a);
    StartRead read{a.pawn.get(), a.token,
                   g_restore_attempt && g_restore_attempt->token == a.token ? g_restore_attempt : nullptr};
    struct ReadScope {
        StartRead* previous = g_start_read;
        explicit ReadScope(StartRead* value) { g_start_read = value; }
        ~ReadScope() { g_start_read = previous; }
    } read_scope(a.queue_owner ? &read : g_start_read);
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
    if (tracked) capture_edge(command, before, reason + 1);
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
    auto* pawn = g_schema.queue_pawn.object(queue);
    const bool remote = native_lily::remote_pawn(pawn);
    if (remote && (!g_restore_attempt || g_restore_attempt->pawn.get() != pawn)) return id;
    const auto ar=reflect::array_at(queue,g_schema.queued);
    if(ar.num<0||ar.num>4096||(ar.num&&!ar.data)){fail("derived command queue layout");return id;}
    for(int i=ar.num-1;i>=0;--i){auto* cmd=reinterpret_cast<ue::UObject**>(ar.data)[i];if(!ue::alive(cmd)||g_schema.command_id.read<std::int32_t>(cmd)!=id)continue;
        const auto h=ObjHandle::of(cmd);
        if (g_restore_attempt && !g_restore_attempt->candidate.valid() && g_restore_attempt->pawn.get() == g_schema.queue_pawn.object(queue)) {
            auto* action = g_schema.action.object(cmd);
            if (action && action->GetClassPrivate() == g_restore_attempt->cls.get()) g_restore_attempt->candidate = h;
        }
        g_pending.erase(h); // admission/reuse always revokes its previous origin
        if(!remote&&g_execution&&g_execution->token&&g_execution->source.domain==0&&g_execution->pawn.get()==pawn){
            prune();if(g_pending.size()>=512){fail("derived command source capacity");return id;}
            g_pending[h]={ObjHandle::of(queue),g_execution->pawn,ObjHandle::of(g_schema.action.object(cmd)),id,g_execution->source,g_execution->token};++g_derived_queued;
        }break;
    }return id;
}
bool install() {
    auto* cls = reflect::find_class(L"/Script/Zenith.CommandAction");
    reflect::Resolver c("command context", cls);
    g_schema.pawn = c.prop(L"Pawn", reflect::Kind::Object);
    g_schema.timeline = c.prop(L"StateTimeline", reflect::Kind::Struct);
    g_schema.consume_mp = c.prop(L"bConsumeMP", reflect::Kind::Bool);
    g_schema.cached_anim = c.prop(L"CachedAnimComponent",reflect::Kind::Object);
    g_schema.invoked_direction=c.func(L"GetInvokedByDirectionInputType",0);g_schema.invoked_button=c.func(L"GetInvokedByInputType",0);
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
    if(!g_sample&&!hook::install_address("CommandSampleNative",sample_target,reinterpret_cast<void*>(&sample_command),reinterpret_cast<void**>(&g_sample),"reflect"))return false;
    if(!g_add&&!hook::install_address("CommandQueueAddNative",add_target,reinterpret_cast<void*>(&add_command),reinterpret_cast<void**>(&g_add),"reflect"))return false;
    if (!g_accept && !hook::install_manifest("CommandAcceptNative", reinterpret_cast<void*>(&accept), reinterpret_cast<void**>(&g_accept))) return false;
    auto* climb = reflect::find_class(L"/Script/Zenith.AutoClimbComponent");
    reflect::Resolver component("climb query", climb);
    g_schema.climb_class = ObjHandle::of(climb);
    g_schema.climb_owner = component.prop(L"CharacterOwner", reflect::Kind::Object);
    const auto getter = component.func(L"GetLastTargetLocation", 0);
    if (!component.ok() || !getter.ok()) return false;
    const auto* getter_exec = reinterpret_cast<const std::uint8_t*>(getter.fn->GetFuncPtr());
    if (!getter_exec || getter_exec[0x23] != 0xE8) return false;
    std::int32_t offset;
    std::memcpy(&offset, getter_exec + 0x24, sizeof(offset));
    auto* native_getter = const_cast<std::uint8_t*>(getter_exec + 0x28 + offset);
    if (native_getter != preflight::address("AutoClimbLastTargetNative")) return false;
    if (!g_read_target && !hook::install_address("AutoClimbLastTargetNative", native_getter,
        reinterpret_cast<void*>(&read_target), reinterpret_cast<void**>(&g_read_target), "reflect")) return false;

    const auto* character = static_cast<const std::uint8_t*>(preflight::address("NativeLilyCharacterTick"));
    if (!character || character[0x12A] != 0xE8) return false;
    std::memcpy(&offset, character + 0x12B, sizeof(offset));
    auto* matcher = const_cast<std::uint8_t*>(character + 0x12F + offset);
    if (matcher != preflight::address("CommandTryAddFromInput")) return false;
    g_character_match_return = character + 0x12F;
    if (!g_match_input && !hook::install_address("CommandTryAddFromInput", matcher,
        reinterpret_cast<void*>(&match_input), reinterpret_cast<void**>(&g_match_input), "character tick")) return false;
    // ProcessCommand removes the interrupted head with reason Override before processing its successor.
    const auto* process = static_cast<const std::uint8_t*>(preflight::address("CommandProcessStart"));
    if (!process || process[0x1E7] != 0xE8) return false;
    std::memcpy(&offset, process + 0x1E8, sizeof(offset));
    const auto* remove = process + 0x1EC + offset;
    if (remove != preflight::address("CommandRemoveNative")) return false;
    g_remove = reinterpret_cast<Remove>(const_cast<std::uint8_t*>(remove));
    g_schema.command_class = ObjHandle::of(cls); g_schema.timeline_class = ObjHandle::of(tl); g_schema.entry_class = ObjHandle::of(en);
    if(!g_post_live){RC::Unreal::Hook::FCallbackOptions o;o.OwnerModName=STR("ELMP");o.HookName=STR("coop.command_context.trace");g_post=RC::Unreal::Hook::RegisterEngineTickPostCallback([](auto&,RC::Unreal::UEngine*,float,bool){sample();},o);g_post_live=true;}
    return true;
}
}
bool enable() { if (g_enabled && g_schema.live()) return true; return g_enabled = install(); }
bool playback_gap() { return g_playback_gap; }
void reset() { g_enabled = false;g_playback_gap=false;if(g_post_live){RC::Unreal::Hook::UnregisterCallback(g_post);g_post_live=false;}g_active.clear();g_pending.clear(); g_schema = {}; g_fault.clear(); }
void forget(ObjHandle pawn) {
    std::erase_if(g_active,[&](const auto& e){return e.second.pawn==pawn;});
    std::erase_if(g_pending,[&](const auto& e){return e.second.pawn==pawn;});
}
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
    reflect::Frame direction(g_schema.invoked_direction),button(g_schema.invoked_button);
    if(!direction.invoke(action)||!button.invoke(action))return p;
    p.cls=action->GetClassPrivate();p.token=it->second.token;p.input=it->second.source.seq;p.direction=direction.ret<std::uint8_t>();p.button=button.ret<std::uint8_t>();return p;
}
std::uint64_t restore(ue::UObject* pawn,ue::UObject* type,std::uint8_t direction,std::uint8_t button) {
    auto* cls = RC::Unreal::Cast<ue::UClass>(type);
    ue::UObject* cdo = cls && cls->IsChildOf(static_cast<ue::UClass*>(g_schema.command_class.get())) ? cls->GetClassDefaultObject() : nullptr;
    if(!eligible(cdo)){fail("Host persistent command is not restorable from its native data");return 0;}
    return play(pawn, type, direction, button);
}
std::uint64_t play(ue::UObject* pawn,ue::UObject* type,std::uint8_t direction,std::uint8_t button,const Target* target) {
    auto* cls = RC::Unreal::Cast<ue::UClass>(type);
    if (!cls || !cls->IsChildOf(static_cast<ue::UClass*>(g_schema.command_class.get()))) {
        fail("invalid Host command class"); return 0;
    }
    auto* queue=queue_of(pawn);
    if (!queue) { fail("Host command queue unavailable"); return 0; }
    RestoreAttempt attempt{ObjHandle::of(pawn), ObjHandle::of(type), 0, {}, target};
    struct Scope {RestoreAttempt* previous=g_restore_attempt;Scope(RestoreAttempt& a){g_restore_attempt=&a;}~Scope(){g_restore_attempt=previous;}} scope(attempt);
    const auto before_starts=g_starts,before_ends=g_ends;
    reflect::Frame add(g_schema.add);if(!add.arg(0,direction).arg(1,button).arg(2,type).invoke(queue)||add.ret<std::int32_t>()<0){fail("persistent command could not enter native queue");return 0;}
    if (target && !attempt.target_read && g_fault.empty()) {
        g_playback_gap = true;
        fail("remote command did not consume the Host climb target");
    }
    if(!attempt.token){
        if (g_fault.empty()) g_playback_gap = true;
        reflect::Frame current(g_schema.current);auto* cmd=current.invoke(queue)?current.ret<ue::UObject*>():nullptr;
        auto* action=cmd?g_schema.action.object(cmd):nullptr;
        fail(std::format("native persistent command did not start: requested={} pawn={} id={} current={} result={} starts={} ends={}",log::utf8(type->GetPathName()),log::utf8(pawn->GetFullName()),add.ret<std::int32_t>(),action?log::utf8(action->GetClassPrivate()->GetPathName()):"<none>",cmd?g_schema.result.read<std::uint8_t>(cmd):0,g_starts-before_starts,g_ends-before_ends));return 0;
    }
    ++g_restored;return attempt.token; // the original zero-delta update may already have ended it
}
bool begin_playback(ue::UObject* pawn) {
    auto* queue = queue_of(pawn);
    if (queue && reflect::Frame(g_schema.clear).invoke(queue)) return true;
    fail("native command queue could not be initialized");
    return false;
}
void finish(ue::UObject* pawn,std::uint64_t token,std::uint8_t reason) {
    auto* action = current_action(pawn);
    const auto it = g_active.find(ObjHandle::of(action));
    if (!action || it == g_active.end() || it->second.token != token) return;
    if (!reason) {
        if (!reflect::Frame(g_schema.request_end).arg_bool(0,false).invoke(action)) fail("native RequestEnd unavailable");
        return;
    }
    auto* queue = queue_of(pawn);
    const auto rows = reflect::array_at(queue, g_schema.queued);
    if (!rows.data || rows.num < 1) return;
    auto* head = *reinterpret_cast<ue::UObject**>(rows.data);
    if (head && g_schema.action.object(head) == action) g_remove(queue, 0, reason);
}
std::string describe() { prune();return std::format("command context: enabled={} active={} starts={} ends={} baselines={} expired={} restored={} climbReads={} trace={} fault='{}'\n  derived queued={} inherited={} pending={} pruned={}\n",g_enabled,g_active.size(),g_starts,g_ends,g_baselines,g_expired,g_restored,g_climb_reads,g_trace.size(),g_fault,g_derived_queued,g_inherited,g_pending.size(),g_pending_pruned); }
std::string trace(std::string_view action) {
    if(action=="on"){g_trace.clear();g_tracing=true;return "command trace enabled\n";}
    if(action=="off"){g_tracing=false;return "command trace disabled\n";}
    auto a=json::Value::array();for(const auto& row:g_trace)a.push(row);return json::dump(a,0)+"\n";
}
}
