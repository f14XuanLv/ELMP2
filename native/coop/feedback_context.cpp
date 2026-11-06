#include "coop/feedback_context.h"

#include "engine/frame.h"
#include "engine/handle.h"
#include "engine/reflect.h"
#include "guard/guard.h"

#include <Unreal/FFrame.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>

namespace elmp::coop::feedback_context {
namespace {
using namespace RC::Unreal;
constexpr auto kPlayer = L"/Game/_Zenith/Core/Pawns/BP_Character_Player_Base.BP_Character_Player_Base_C";
constexpr auto kContact = L"BndEvt__Collision_K2Node_ComponentBoundEvent_5_CollisionEvent__DelegateSignature";
constexpr auto kContactPath = L"/Game/_Zenith/Core/Pawns/BP_Character_Player_Base.BP_Character_Player_Base_C:BndEvt__Collision_K2Node_ComponentBoundEvent_5_CollisionEvent__DelegateSignature";
constexpr std::size_t kDepth = 64, kOuterDepth = 32;
bool g_installed = false, g_enabled = false;
std::atomic<UFunction*> g_contact{}; // cold-resolved pointer for the per-script-call comparison
ObjHandle g_contact_handle, g_player;
Hook::GlobalCallbackId g_pre{}, g_post{};
std::string g_failure, g_fault;

enum class Origin : std::uint8_t { Apply, Finalize, Contact };
struct Scope {
    Origin origin{};
    ObjHandle self, from, target;
    // Raw identities only pair this synchronous call's exit; never dereferenced or used as authority.
    std::uintptr_t self_id = 0, from_id = 0, target_id = 0, frame_id = 0;
};
std::array<Scope, kDepth> g_stack{};
std::size_t g_depth = 0, g_peak = 0;
std::uint64_t g_apply = 0, g_finalize = 0, g_contact_enter = 0, g_exits = 0;
std::uint64_t g_owned = 0, g_unmapped = 0, g_expired = 0, g_resolves = 0, g_invalidated = 0, g_faults = 0;

void fail(std::string reason) {
    g_failure = std::move(reason);
    if (g_fault.empty()) { g_fault = g_failure; ++g_faults; }
}
std::uintptr_t identity(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

bool resolve() {
    if (g_contact_handle.valid() && g_player.valid()) return true;
    if (!g_contact_handle.empty() || !g_player.empty()) ++g_invalidated;
    g_contact = nullptr; g_contact_handle.reset(); g_player.reset();
    ++g_resolves;
    auto* player = reflect::find_class(kPlayer);
    reflect::Resolver r("Feedback Player contact", player);
    auto fn = r.func(kContact, 1);
    if (!r.ok()) { g_failure = r.failure(); return false; }
    // E-CUE contact-function.json: exactly one Actor* OverlapActor, no return, 8-byte parameter frame.
    if (fn.fn != reflect::find_object(kContactPath) || fn.fn->GetOuterPrivate() != player ||
        fn.ret >= 0 || fn.count != 1 || fn.parms_size != 8 || fn.parms[0].kind != reflect::Kind::Object ||
        fn.parms[0].size != 8 || fn.parms[0].offset != 0 || fn.parms[0].prop->GetName() != L"OverlapActor") {
        g_failure = "Feedback exact PlayerBase contact signature/owner mismatch"; return false;
    }
    g_player = ObjHandle::of(player); g_contact_handle = fn.handle; g_contact = fn.fn;
    g_failure.clear(); return true;
}

void enter(Origin origin, UObject* self, UObject* from, UObject* target, const void* frame = nullptr) {
    if (!g_enabled || !g_fault.empty()) return;
    if (g_depth == g_stack.size()) return fail("Feedback nested source scope capacity exceeded");
    if (!ue::alive(self)) return fail("Feedback source self expired at entry");
    Scope s;
    s.origin = origin;
    s.self = ObjHandle::of(self);
    if (ue::alive(from)) s.from = ObjHandle::of(from);
    if (ue::alive(target)) s.target = ObjHandle::of(target);
    s.self_id = identity(self); s.from_id = identity(from); s.target_id = identity(target); s.frame_id = identity(frame);
    g_stack[g_depth++] = s; g_peak = std::max(g_peak, g_depth);
    if (origin == Origin::Apply) ++g_apply;
    else if (origin == Origin::Finalize) ++g_finalize;
    else ++g_contact_enter;
}
void exit_scope(Origin origin, UObject* self, UObject* from, UObject* target, const void* frame = nullptr) {
    if (!g_enabled || !g_fault.empty()) return;
    if (!g_depth) return fail("Feedback source scope exit without entry");
    const auto& s = g_stack[g_depth - 1];
    if (s.origin != origin || s.self_id != identity(self) || s.from_id != identity(from) ||
        s.target_id != identity(target) || s.frame_id != identity(frame))
        return fail("Feedback source scope nesting mismatch");
    g_stack[--g_depth] = {}; ++g_exits;
}
void effect(bool entering, UObject* self, UObject* from, UObject* to, bool finalize) {
    if (!ue::on_game_thread()) return;
    const auto origin = finalize ? Origin::Finalize : Origin::Apply;
    if (entering) enter(origin, self, from, to);
    else exit_scope(origin, self, from, to);
}
bool contact(UObject* actor, FFrame& stack) {
    if (!g_enabled || !ue::on_game_thread()) return false;
    auto* node = stack.Node();
    if (!node || node != g_contact.load(std::memory_order_relaxed)) return false;
    if (g_contact_handle.get() != node || !g_player.valid() || !ue::alive(actor) ||
        !actor->GetClassPrivate()->IsChildOf(static_cast<ue::UClass*>(g_player.get()))) {
        fail("Feedback contact function/actor identity expired"); return false;
    }
    return true;
}
void on_pre(Hook::TCallbackIterationData<void>&, UObject* actor, FFrame& stack, void*) {
    if (contact(actor, stack)) enter(Origin::Contact, actor, nullptr, actor, &stack);
}
void on_post(Hook::TCallbackIterationData<void>&, UObject* actor, FFrame& stack, void*) {
    if (!g_enabled || !ue::on_game_thread() || stack.Node() != g_contact.load(std::memory_order_relaxed)) return;
    // Actor may have been destroyed by the original callback. Pair its raw identity without dereference.
    exit_scope(Origin::Contact, actor, nullptr, actor, &stack);
}
const Scope* current() {
    if (!g_enabled || !ue::on_game_thread() || !g_fault.empty() || !g_depth) return nullptr;
    const auto& s = g_stack[g_depth - 1];
    if (!s.self.valid()) { ++g_expired; fail("Feedback active source self expired"); return nullptr; }
    return &s;
}
bool outer_reaches(UObject* source, UObject* target) {
    if (!ue::alive(target)) return false;
    for (std::size_t i = 0; source && i < kOuterDepth; ++i) {
        if (!ue::alive(source)) return false;
        if (source == target) return true;
        auto* next = source->GetOuterPrivate();
        if (next == source) return false;
        source = next;
    }
    return false;
}
void refresh(const frame::Tick&) {
    if (g_depth) return fail("Feedback source scope remained open across a frame boundary");
    // Missing classes during a map transition are retried, never cached as a permanent resolve failure.
    resolve();
}
} // namespace

void install() {
    if (g_installed) return;
    g_installed = true;
    frame::add_phase("feedback_context", refresh, [] { return g_enabled; }, 1000);
}
bool enable() {
    if (g_enabled) return g_fault.empty() && g_contact_handle.valid() && g_player.valid();
    if (!g_installed) { g_failure = "Feedback install must run during initialization"; return false; }
    if (!resolve()) return false;
    Hook::FCallbackOptions options;
    options.OwnerModName = STR("ELMP"); options.HookName = STR("coop.feedback.contact"); options.bReadonly = true;
    g_pre = Hook::RegisterProcessLocalScriptFunctionPreCallback(on_pre, options);
    g_post = Hook::RegisterProcessLocalScriptFunctionPostCallback(on_post, options);
    if (!g_pre || !g_post || !guard::observe_effects(effect)) {
        if (g_pre) Hook::UnregisterCallback(g_pre);
        if (g_post) Hook::UnregisterCallback(g_post);
        g_pre = g_post = 0;
        g_failure = "Feedback contact callbacks or Effect observer installation failed"; return false;
    }
    g_stack = {}; g_depth = 0; g_fault.clear(); g_enabled = true;
    return true;
}
void disable() {
    g_enabled = false; // no callback calls disable itself; removal occurs from the session/frame lifecycle
    guard::observe_effects(nullptr);
    if (g_pre) Hook::UnregisterCallback(g_pre);
    if (g_post) Hook::UnregisterCallback(g_post);
    g_pre = g_post = 0;
    g_contact = nullptr; g_contact_handle.reset(); g_player.reset();
    g_stack = {}; g_depth = 0; g_fault.clear();
}
bool scoped() { return g_enabled && ue::on_game_thread() && g_depth != 0; }
UObject* effect_source() { const auto* s=current();return s&&s->origin!=Origin::Contact?s->self.get():nullptr; }
bool effect_scope() { return scoped() && g_stack[g_depth - 1].origin != Origin::Contact; }
UObject* sound_target(UObject* ctx) {
    const auto* s = current();
    if (!s) return nullptr;
    auto* target = s->target.get();
    if (target && ((s->origin != Origin::Contact && s->self.get() == ctx) || outer_reaches(ctx, target))) {
        ++g_owned; return target;
    }
    ++g_unmapped; return nullptr;
}
bool owns(UObject* source) {
    const auto* s = current();
    if (!s) return false;
    if ((s->origin != Origin::Contact && s->self.get() == source) || outer_reaches(source, s->target.get())) {
        ++g_owned; return true;
    }
    ++g_unmapped; return false;
}
std::string fault() {
    if (!g_fault.empty()) return g_fault;
    if (g_enabled && (!g_contact_handle.valid() || !g_player.valid()))
        return g_failure.empty() ? "Feedback exact contact function unavailable" : g_failure;
    return {};
}
std::string describe() {
    return std::format("\n  feedbackContext enabled={} contactReady={} callbacks={}/{} depth={} peak={} apply={} finalize={} contact={} exits={} owned={} unmapped={} expired={} resolves={} invalidated={} faults={} failure={} fault={}",
        g_enabled, g_contact_handle.valid() && g_player.valid(), g_pre != 0, g_post != 0, g_depth, g_peak,
        g_apply, g_finalize, g_contact_enter, g_exits, g_owned, g_unmapped, g_expired, g_resolves, g_invalidated, g_faults, g_failure, fault());
}

} // namespace elmp::coop::feedback_context
