#include "pvp/hitgate.h"

#include "core/hitrules.h"
#include "core/player.h"
#include "core/schema.h"

#include "engine/frame.h"
#include "engine/handle.h"
#include "engine/reflect.h"
#include "engine/world.h"

#include "base/hook.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/settings.h"

#include <Unreal/FFrame.hpp>
#include <Unreal/UFunctionStructs.hpp>

#include <array>
#include <cstring>
#include <format>
#include <optional>
#include <vector>

namespace elmp::pvp {

using namespace RC::Unreal;
using reflect::Kind;
using reflect::PropRef;

namespace {

constexpr std::uint8_t kFactionEnemies = 2;

struct Layout {
    UClass *actor, *pawn, *controller, *character, *ability, *param_component, *knockback;
    std::int32_t owner, instigator, pawn_controller, invoker, faction_component, faction, param, damage_bonus;
    PropRef ae_active, kb_bypass;
    std::int32_t kb_data, kb_size;
    std::array<std::int32_t, 3> kb_vectors;
    std::int32_t kb_stun, kb_move, kb_strength;
    reflect::FuncRef components;
};
std::optional<Layout> g_layout;
std::string g_failure;

struct Bound {
    ObjHandle pc;
    core::hitrules::Team team;
};
std::vector<Bound> g_bound;
std::uint64_t g_bound_generation = 0;
bool g_active = false; // opposing pair exists and the layout is armed

struct KbPatch {
    std::uint8_t* base;
    std::uint8_t bypass_byte;
    std::vector<std::uint8_t> data;
};
struct Window {
    ObjHandle component;
    std::uint8_t saved = 0;
    bool flipped = false;
    float* bonus = nullptr;
    float bonus_saved = 0;
    std::vector<KbPatch> kb;
};
std::vector<Window> g_stack;

using EffectQueue = std::uintptr_t(__fastcall*)(UObject*, std::uint8_t*, float);
EffectQueue g_effect_queue{};

struct FnHook {
    UFunction* fn = nullptr;
    CallbackId pre = 0, post = 0;
    std::int32_t offset = -1, steps = -1;
};
std::array<FnHook, 3> g_hooks{};
bool g_hooked = false;

// Profiles: lateral X / up Z / stun / move time / strength (medium ≈ e2000 basic attack).
struct Knock {
    float vx, vz, stun, move, strength;
};
constexpr Knock kProfiles[] = {{0, 0, 0, 0, 0}, {250, 350, 0.15f, 0.20f, 0}, {500, 500, 0.25f, 0.30f, 0}, {700, 650, 0.40f, 0.45f, 0}};

metrics::Counter& c_calls = metrics::counter("pvp.calls");
metrics::Counter& c_flips = metrics::counter("pvp.flips");
metrics::Counter& c_scales = metrics::counter("pvp.scales");
metrics::Counter& c_knocks = metrics::counter("pvp.knocks");
metrics::Counter& c_effects = metrics::counter("pvp.effects");

template <class T>
T read(const UObject* o, std::int32_t offset) {
    T v;
    std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(o) + offset, sizeof(T));
    return v;
}

bool is(const UObject* o, UClass* cls) { return o && cls && o->GetClassPrivate()->IsChildOf(cls); }

const Bound* match(const UObject* pc) {
    if (!pc) return nullptr;
    const auto h = ObjHandle::of(pc);
    for (const auto& b : g_bound)
        if (b.pc == h) return &b;
    return nullptr;
}

// Seat of a hit participant: controller itself, a pawn's controller, or (summons / spirits / projectiles)
// up the Owner → Instigator chain. Unresolved = not a player: vanilla behaviour.
const Bound* seat_of(UObject* o) {
    const auto& L = *g_layout;
    for (int hop = 0; o && hop < 6; ++hop) {
        if (is(o, L.controller)) return match(o);
        if (is(o, L.pawn))
            if (const auto* b = match(read<UObject*>(o, L.pawn_controller))) return b;
        if (!is(o, L.actor)) return nullptr;
        UObject* next = read<UObject*>(o, L.owner);
        if (!next) next = read<UObject*>(o, L.instigator);
        if (next == o) return nullptr;
        o = next;
    }
    return nullptr;
}

void flip(Window& w, UObject* target) {
    const auto& L = *g_layout;
    if (!is(target, L.character)) return;
    auto* comp = read<UObject*>(target, L.faction_component);
    if (!ue::alive(comp)) return;
    auto* f = reinterpret_cast<std::uint8_t*>(comp) + L.faction;
    w.component = ObjHandle::of(comp);
    w.saved = *f;
    w.flipped = true;
    *f = kFactionEnemies; // field write: no SetFaction, no OnFactionChange broadcast
    ++c_flips;
}

void scale_damage(Window& w, const Bound& attacker, double scale) {
    const auto& L = *g_layout;
    if (scale >= 1.0) return;
    auto* pc = attacker.pc.get();
    auto* comp = pc ? read<UObject*>(pc, L.param) : nullptr;
    if (!is(comp, L.param_component)) return;
    auto* bonus = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(comp) + L.damage_bonus);
    for (const auto& outer : g_stack) // nested windows / multi-target swings: apply once per attacker
        if (outer.bonus == bonus) return;
    w.bonus = bonus;
    w.bonus_saved = *bonus;
    *bonus += static_cast<float>((scale - 1.0) * 100.0);
    ++c_scales;
}

void write_profile(std::uint8_t* data, const Knock& p) {
    const auto& L = *g_layout;
    for (const auto off : L.kb_vectors) {
        const float v[3] = {p.vx, 0.0f, p.vz};
        std::memcpy(data + off, v, sizeof(v));
    }
    std::memcpy(data + L.kb_stun, &p.stun, 4);
    std::memcpy(data + L.kb_move, &p.move, 4);
    std::memcpy(data + L.kb_strength, &p.strength, 4);
}

// Vanilla AE_Knockback only launches once the target's stamina is broken (or bBypassStaminaCheck);
// bActive is the vanilla "this attack knocks back" flag. Level 1..3: bypass + profile data, restored after.
void knockback(Window& w, UObject* ability, int level) {
    const auto& L = *g_layout;
    if (level < 1 || level > 3 || !is(ability, L.actor)) return;
    reflect::Frame f(L.components);
    f.arg(0, L.knockback);
    if (!f.invoke(ability)) return;
    const auto* arr = static_cast<const reflect::RawArray*>(f.raw(L.components.ret));
    if (!arr || arr->num <= 0 || arr->num > 16) return;
    const auto* items = reinterpret_cast<UObject* const*>(arr->data);
    for (int i = 0; i < arr->num; ++i) {
        if (!ue::alive(items[i]) || !L.ae_active.read_bool(items[i])) continue;
        auto* base = reinterpret_cast<std::uint8_t*>(items[i]);
        bool patched = false;
        for (const auto& outer : g_stack)
            for (const auto& p : outer.kb) patched |= p.base == base;
        if (patched) continue;
        KbPatch p{base, L.kb_bypass.at<std::uint8_t>(base)[L.kb_bypass.byte_offset], {}};
        p.data.assign(base + L.kb_data, base + L.kb_data + L.kb_size);
        L.kb_bypass.write_bool(base, true);
        write_profile(base + L.kb_data, kProfiles[level]);
        w.kb.push_back(std::move(p));
        ++c_knocks;
    }
}

// Every window pushes exactly one record; the matching end pops it.
void begin(UObject* ability, UObject* target) {
    auto& w = g_stack.emplace_back();
    if (!g_active || !ability || !target || !ue::on_game_thread()) return;
    const auto& L = *g_layout;
    UObject* invoker = is(ability, L.ability) ? read<UObject*>(ability, L.invoker) : nullptr;
    const Bound* attacker = seat_of(invoker);
    if (!attacker) return;
    const Bound* victim = seat_of(target);
    if (!victim || victim == attacker || victim->team == attacker->team) return;
    const auto rule = core::hitrules::rule();
    flip(w, target);
    scale_damage(w, *attacker, rule.damage);
    knockback(w, ability, rule.knockback);
}

void end() {
    if (g_stack.empty()) return;
    auto w = std::move(g_stack.back());
    g_stack.pop_back();
    if (w.flipped)
        if (auto* comp = w.component.get()) reinterpret_cast<std::uint8_t*>(comp)[g_layout->faction] = w.saved;
    if (w.bonus) *w.bonus = w.bonus_saved;
    for (auto it = w.kb.rbegin(); it != w.kb.rend(); ++it) {
        it->base[g_layout->kb_bypass.offset + g_layout->kb_bypass.byte_offset] = it->bypass_byte;
        std::memcpy(it->base + g_layout->kb_data, it->data.data(), it->data.size());
    }
}

// ProcessEvent path: Locals is the parameter block. Bytecode path: step to the parameter, then restore
// Code / property chain / OutParms so the native implementation reads its arguments normally.
UObject* target_of(FFrame& stack, const FnHook& h) {
    if (!stack.Code()) return stack.Locals() ? *reinterpret_cast<UObject**>(stack.Locals() + h.offset) : nullptr;
    auto* code = stack.Code();
    auto* chain = stack.PropertyChainForCompiledIn();
    auto* out = stack.OutParms();
    UObject* v = nullptr;
    for (int i = 0; i <= h.steps; ++i) {
        v = nullptr;
        stack.StepCompiledIn<FObjectProperty>(&v);
    }
    stack.Code() = code;
    stack.PropertyChainForCompiledIn() = chain;
    stack.OutParms() = out;
    return v;
}

std::uintptr_t __fastcall effect_queue(UObject* ability, std::uint8_t* entry, float dt) {
    if (!g_active) return g_effect_queue(ability, entry, dt);
    ++c_effects;
    begin(ability, entry ? reinterpret_cast<FWeakObjectPtr*>(entry)->Get() : nullptr); // FHitEntry +0: target
    const auto r = g_effect_queue(ability, entry, dt);
    end();
    return r;
}

std::optional<Layout> resolve() {
    using reflect::Resolver;
    const auto* s = core::schema::script();
    if (!s) {
        g_failure = "core schema: " + core::schema::status();
        return std::nullopt;
    }
    Layout L{};
    L.actor = reflect::find_class(L"/Script/Engine.Actor");
    L.pawn = reflect::find_class(L"/Script/Engine.Pawn");
    L.controller = reflect::find_class(L"/Script/Engine.Controller");
    L.character = reflect::find_class(L"/Script/Zenith.ZenithCharacter");
    L.ability = reflect::find_class(L"/Script/Zenith.Ability");
    L.param_component = reflect::find_class(L"/Script/Zenith.ParameterPlayerComponent");
    L.knockback = reflect::find_class(L"/Script/Zenith.AE_KnockbackComponent");
    Resolver actor("Actor", L.actor), ability("Ability", L.ability), param("ParameterPlayerComponent", L.param_component), kb("AE_KnockbackComponent", L.knockback);
    L.owner = actor.prop(L"Owner", Kind::Object).offset;
    L.instigator = actor.prop(L"Instigator", Kind::Object).offset;
    L.components = actor.func(L"K2_GetComponentsByClass", 1);
    L.invoker = ability.prop(L"Invoker", Kind::Object).offset;
    L.damage_bonus = param.prop(L"DamagePercentBonus", Kind::Float, 4).offset;
    L.ae_active = kb.prop(L"bActive", Kind::Bool);
    L.kb_bypass = kb.prop(L"bBypassStaminaCheck", Kind::Bool);
    const auto data = kb.prop(L"KnockbackData", Kind::Struct);
    for (const auto* r : {&actor, &ability, &param, &kb})
        if (!r->ok()) {
            g_failure = r->failure();
            return std::nullopt;
        }
    auto* sp = static_cast<FStructProperty*>(data.prop);
    auto* st = static_cast<ue::UStruct*>(sp->GetStruct().Get());
    Resolver kd("KnockbackData", st);
    const std::array<const wchar_t*, 3> vectors{L"LaunchVectorGrounded", L"LaunchVectorAir", L"LaunchVectorSwim"};
    for (int i = 0; i < 3; ++i) L.kb_vectors[i] = kd.prop(vectors[i], Kind::Struct, 12).offset;
    L.kb_stun = kd.prop(L"StunTime", Kind::Float, 4).offset;
    L.kb_move = kd.prop(L"MoveTime", Kind::Float, 4).offset;
    L.kb_strength = kd.prop(L"Strength", Kind::Float, 4).offset;
    if (!kd.ok()) {
        g_failure = kd.failure();
        return std::nullopt;
    }
    L.kb_data = data.offset;
    L.kb_size = reflect::properties_size(st);
    L.pawn_controller = s->pawn_controller.offset;
    L.faction_component = s->zc_faction.offset;
    L.faction = s->faction_value.offset;
    L.param = s->zpc_param.offset;
    if (!L.pawn || !L.controller || !L.character || L.kb_size < 48) {
        g_failure = "pvp class layout mismatch";
        return std::nullopt;
    }
    return L;
}

bool resolve_param(FnHook& h, const wchar_t* name) {
    int index = 0;
    for (auto* p : TFieldRange<FProperty>(h.fn, EFieldIterationFlags::Default)) {
        const bool parm = p->HasAnyPropertyFlags(CPF_Parm) && !p->HasAnyPropertyFlags(CPF_ReturnParm);
        if (!parm) continue;
        if (reflect::same_name(p->GetFName().ToString(), name)) {
            if (!p->IsA<FObjectProperty>()) return false;
            h.offset = p->GetOffset_Internal();
            h.steps = index;
            return true;
        }
        ++index;
    }
    return false;
}

bool arm() {
    if (g_hooked) return true;
    if (!g_failure.empty()) return false; // refusals are final for this process (fail closed)
    g_layout = resolve();
    if (!g_layout) {
        log::error("pvp hit windows disabled: {}", g_failure);
        return false;
    }
    constexpr std::array<std::pair<const wchar_t*, const wchar_t*>, 3> entries{{
        {L"/Script/Zenith.AbilityHitbox:OnActorOverlapBegin", L"OtherActor"},
        {L"/Script/Zenith.AbilityHitbox:ProcessActorEnter", L"EnterActor"},
        {L"/Script/Zenith.Ability:ExecuteAbilityEffectsOnTarget", L"Target"},
    }};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto& h = g_hooks[i];
        h.fn = static_cast<UFunction*>(reflect::find_object(entries[i].first));
        if (!h.fn || !h.fn->HasAnyFunctionFlags(FUNC_Native) || !resolve_param(h, entries[i].second)) {
            g_failure = "hit entry unusable: " + log::utf8(entries[i].first);
            log::error("pvp hit windows disabled: {}", g_failure);
            return false;
        }
    }
    if (!hook::install_manifest("AbilityEffectQueue", reinterpret_cast<void*>(&effect_queue), reinterpret_cast<void**>(&g_effect_queue))) {
        g_failure = "AbilityEffectQueue hook refused (see preflight)";
        log::error("pvp hit windows disabled: {}", g_failure);
        return false;
    }
    for (auto& h : g_hooks) {
        const FnHook* p = &h;
        h.pre = h.fn->RegisterPreHook([p](UnrealScriptFunctionCallableContext& c, void*) {
            ++c_calls;
            begin(c.Context, target_of(c.TheStack, *p));
        });
        h.post = h.fn->RegisterPostHook([](UnrealScriptFunctionCallableContext&, void*) { end(); });
    }
    g_hooked = true;
    log::info("pvp: hit windows armed (3 entries + effect queue)");
    return true;
}

void rebuild() {
    g_bound.clear();
    for (const auto& b : core::hitrules::bindings())
        if (auto* pc = b.slot == 1 ? world::p1_controller() : core::controller(b.slot)) g_bound.push_back({ObjHandle::of(pc), b.team});
    g_bound_generation = core::hitrules::generation();
}

void push_rule() {
    core::hitrules::set_rule({settings::get_number("pvpDamage"), static_cast<int>(settings::get_number("pvpKnockback"))});
}

// 250 ms: pooled controllers are stable per world, P1's controller only changes with the world.
void step(const frame::Tick&) {
    const bool want = core::hitrules::opposed();
    push_rule(); // settings.json is loaded after install; cheap and change-guarded
    if (want && !arm()) {
        g_active = false;
        return;
    }
    if (want && (g_bound_generation != core::hitrules::generation() || g_bound.empty() || !g_bound.front().pc.valid())) rebuild();
    g_active = want;
    if (!want) g_bound.clear();
}

} // namespace

void install() {
    push_rule();
    settings::on_change([](const std::string& key) {
        if (key == "pvpDamage" || key == "pvpKnockback") push_rule();
    });
    frame::add_phase("pvp.bind", step, [] { return core::hitrules::opposed() || g_active; }, 250);
}

void uninstall() {
    g_active = false;
    for (auto& h : g_hooks) {
        if (!h.fn) continue;
        if (h.pre) h.fn->UnregisterHook(h.pre);
        if (h.post) h.fn->UnregisterHook(h.post);
        h = {};
    }
    g_stack.clear();
}

Status status() { return {g_hooked, g_active, g_failure, c_calls.load(), c_flips.load(), c_scales.load(), c_knocks.load(), c_effects.load()}; }

std::string describe() {
    const auto st = status();
    const auto r = core::hitrules::rule();
    std::string teams;
    for (const auto& b : core::hitrules::bindings()) teams += std::format(" P{}={}", b.slot, core::hitrules::team_name(b.team));
    return std::format("hooked={} active={} damage={:.2f} knockback={} calls={} flips={} scales={} knocks={} effects={} stack={}{}\nteams:{}\n", st.hooked,
                       st.active, r.damage, r.knockback, st.calls, st.flips, st.scales, st.knocks, st.effects, g_stack.size(),
                       st.failure.empty() ? "" : " failure: " + st.failure, teams);
}

} // namespace elmp::pvp
