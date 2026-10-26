#include "core/schema.h"

#include "engine/lifecycle.h"

#include "base/clock.h"
#include "base/log.h"

#include <optional>

namespace elmp::core::schema {

using reflect::Kind;
using reflect::Resolver;

namespace {

std::optional<Script> g_script;
std::string g_script_failure = "not resolved yet";
bool g_script_tried = false;

std::optional<Blueprint> g_bp;
std::string g_bp_failure = "not resolved yet";
std::uint64_t g_bp_generation = ~0ull;
std::int64_t g_bp_tried_us = 0;

std::string g_optional_missing;

struct Collect {
    std::string why;
    void take(const Resolver& r) {
        if (!r.ok() && why.empty()) why = r.failure();
    }
};

ue::UStruct* cls(const wchar_t* path) { return static_cast<ue::UStruct*>(reflect::find_object(path)); }

bool resolve_script() {
    Script s;
    Collect c;
    {
        Resolver r("Actor", cls(L"/Script/Engine.Actor"));
        s.actor_tags = r.prop(L"Tags", Kind::Array);
        s.actor_root = r.prop(L"RootComponent", Kind::Object);
        s.actor_destroying = r.prop(L"bActorIsBeingDestroyed", Kind::Bool);
        s.actor_destroy = r.func(L"K2_DestroyActor", 0);
        s.actor_disable_input = r.func(L"DisableInput", 1);
        s.actor_location = r.func(L"K2_GetActorLocation", 0);
        s.actor_teleport = r.func(L"K2_SetActorLocation", 4);
        c.take(r);
    }
    {
        Resolver r("SceneComponent", cls(L"/Script/Engine.SceneComponent"));
        s.scene_rel_location = r.prop(L"RelativeLocation", Kind::Struct, 12);
        c.take(r);
    }
    {
        Resolver r("PrimitiveComponent", cls(L"/Script/Engine.PrimitiveComponent"));
        s.prim_set_material = r.func(L"SetMaterial", 2);
        c.take(r);
    }
    {
        Resolver r("Pawn", cls(L"/Script/Engine.Pawn"));
        s.pawn_controller = r.prop(L"Controller", Kind::Object);
        c.take(r);
    }
    {
        Resolver r("Controller", cls(L"/Script/Engine.Controller"));
        s.controller_pawn = r.prop(L"Pawn", Kind::Object);
        s.controller_possess = r.func(L"Possess", 1);
        s.controller_unpossess = r.func(L"UnPossess", 0);
        c.take(r);
    }
    {
        Resolver r("PlayerController", cls(L"/Script/Engine.PlayerController"));
        s.pc_player = r.prop(L"Player", Kind::Object);
        s.pc_camera_class = r.prop(L"PlayerCameraManagerClass", Kind::Class);
        c.take(r);
    }
    {
        Resolver r("ZenithPlayerController", cls(L"/Script/Zenith.ZenithPlayerController"));
        s.zpc_inventory = r.prop(L"InventoryComponent", Kind::Object);
        s.zpc_param = r.prop(L"ParameterPlayerComponent", Kind::Object);
        s.zpc_spirit_equip = r.prop(L"SpiritEquipComponent", Kind::Object);
        s.zpc_heal = r.prop(L"HealComponent", Kind::Object);
        s.zpc_passive = r.prop(L"PassiveEquipComponent", Kind::Object);
        s.zpc_fully_restore = r.func(L"FullyRestore", 1);
        s.zpc_aptitude_unlocked = r.func(L"IsAptitudeUnlocked", 1);
        s.zpc_unlocked = r.prop(L"UnlockedAptitudes", Kind::Array);
        c.take(r);
    }
    {
        Resolver r("ZenithCharacter", cls(L"/Script/Zenith.ZenithCharacter"));
        s.zc_input = r.prop(L"ZenithInputComponent", Kind::Object);
        s.zc_hp = r.prop(L"HPComponent", Kind::Object);
        s.zc_mp = r.prop(L"MPComponent", Kind::Object);
        s.zc_faction = r.prop(L"FactionComponent", Kind::Object);
        s.zc_command_queue = r.prop(L"CommandQueueComponent", Kind::Object);
        s.zc_locomotion = r.prop(L"LocomotionComponent", Kind::Object);
        c.take(r);
        Resolver l("LocomotionComponent", cls(L"/Script/Zenith.LocomotionComponent"));
        s.loco_facing_right = l.func(L"IsFacingRight", 0);
        c.take(l);
    }
    {
        Resolver r("ZenithInputComponent", cls(L"/Script/Zenith.ZenithInputComponent"));
        s.zic_pawn_owner = r.prop(L"PawnOwner", Kind::Object);
        s.zic_snapshot = r.prop(L"InputSnapshot", Kind::Struct, 40);
        c.take(r);
    }
    {
        Resolver r("HPComponent", cls(L"/Script/Zenith.HPComponent"));
        s.hp_curr = r.prop(L"CurrHP", Kind::Int, 4);
        s.hp_max = r.prop(L"MaxHP", Kind::Int, 4);
        s.hp_alive = r.func(L"IsAlive", 0);
        c.take(r);
    }
    {
        Resolver r("MPComponent", cls(L"/Script/Zenith.MPComponent"));
        s.mp_curr = r.prop(L"CurrMP", Kind::Int, 4);
        s.mp_max = r.prop(L"MaxMP", Kind::Int, 4);
        s.mp_add = r.func(L"Add", 1);
        s.mp_reset = r.func(L"Reset", 0);
        c.take(r);
    }
    {
        Resolver r("HealComponent", cls(L"/Script/Zenith.HealComponent"));
        s.heal_max_count_level = r.prop(L"MaxHealCountLevel", Kind::Int, 4);
        s.heal_remaining = r.func(L"GetRemainingHealCount", 0);
        s.heal_max_count = r.func(L"GetMaxHealCount", 0);
        s.heal_count_level = r.func(L"GetHealCountLevel", 0);
        s.heal_power_level = r.func(L"GetHealPowerLevel", 0);
        s.heal_set_count_level = r.func(L"SetHealCountLevel", 1);
        s.heal_set_power_level = r.func(L"SetHealPowerLevel", 1);
        s.heal_fully_restore = r.func(L"FullyRestoreHeals", 0);
        c.take(r);
    }
    {
        Resolver r("CommandQueueComponent", cls(L"/Script/Zenith.CommandQueueComponent"));
        s.cq_has_command = r.func(L"HasCommand", 0);
        c.take(r);
    }
    {
        Resolver r("FactionComponent", cls(L"/Script/Zenith.FactionComponent"));
        s.faction_value = r.prop(L"Faction", Kind::Any, 1);
        c.take(r);
    }
    {
        Resolver r("InventoryComponent", cls(L"/Script/Zenith.InventoryComponent"));
        s.inv_add_item = r.func(L"AddItem", 1);
        s.inv_aptitudes = r.func(L"GetItemAptitudeInventory", 0);
        s.inv_spirits = r.func(L"GetItemSpiritInventory", 0);
        s.inv_passives = r.func(L"GetItemPassiveInventory", 0);
        s.inv_parameters = r.func(L"GetItemParameterInventory", 0);
        c.take(r);
    }
    {
        Resolver r("BaseInventory", cls(L"/Script/Zenith.BaseInventory"));
        s.binv_count = r.func(L"GetItemCount", 0);
        s.binv_handle_at = r.func(L"GetItemHandleAtIndex", 2);
        s.binv_remove = r.func(L"RemoveItem", 1);
        c.take(r);
    }
    {
        Resolver r("ItemSpiritInventory", cls(L"/Script/Zenith.ItemSpiritInventory"));
        s.spirit_levels = r.prop(L"SpiritLevels", Kind::Map);
        s.spirit_set_level = r.func(L"SetSpiritLevel", 2);
        s.spirit_get_level = r.func(L"GetSpiritLevel", 2);
        c.take(r);
    }
    {
        Resolver r("SpiritEquipComponent", cls(L"/Script/Zenith.SpiritEquipComponent"));
        s.se_sets = r.prop(L"SummonSets", Kind::Map);
        s.se_equip = r.func(L"EquipSpirit", 3);
        s.se_unequip = r.func(L"UnequipSpirit", 2);
        s.se_unequip_all = r.func(L"UnequipAllSpirits", 0);
        s.se_switch = r.func(L"SwitchSummonSet", 1);
        s.se_current = r.func(L"GetCurrentSummonSet", 0);
        s.se_reequip = r.func(L"ReequipSpirits", 0);
        c.take(r);
    }
    {
        Resolver r("SummonSetData", cls(L"/Script/Zenith.SummonSetData"));
        s.set_spirits = r.prop(L"Spirits", Kind::Map);
        c.take(r);
        Resolver d("SpiritData", cls(L"/Script/Zenith.SpiritData"));
        s.spirit_id = d.prop(L"SpiritID", Kind::Name, 8);
        c.take(d);
    }
    {
        Resolver r("PassiveEquipComponent", cls(L"/Script/Zenith.PassiveEquipComponent"));
        s.pe_equipped = r.prop(L"EquippedPassives", Kind::Array);
        s.pe_equip = r.func(L"EquipPassive", 1);
        s.pe_unequip = r.func(L"UnequipPassive", 1);
        s.pe_is_equipped = r.func(L"IsPassiveEquipped", 1);
        s.pe_slot_count = r.func(L"GetSlotCount", 0);
        s.pe_set_slot_count = r.func(L"SetSlotCount", 1);
        c.take(r);
        Resolver d("PassiveEquipRuntimeData", cls(L"/Script/Zenith.PassiveEquipRuntimeData"));
        s.passive_id = d.prop(L"PassiveID", Kind::Name, 8);
        c.take(d);
    }
    {
        Resolver r("ParameterPlayerComponent", cls(L"/Script/Zenith.ParameterPlayerComponent"));
        s.pp_experience = r.prop(L"ExperiencePoints", Kind::Int, 4);
        s.pp_level = r.func(L"GetLevel", 1);
        s.pp_set_level = r.func(L"SetLevel", 1);
        s.pp_need_exp = r.func(L"GetNecessaryExpPointsForLevelUp", 0);
        s.pp_add_max_hp_bonus = r.func(L"AddMaxHPBonusPercentage", 1);
        c.take(r);
    }
    {
        Resolver r("SpineSkeletonAnimationExComponent", cls(L"/Script/SpineExtensionPlugin.SpineSkeletonAnimationExComponent"));
        s.sk_atlas = r.prop(L"Atlas", Kind::Object);
        s.sp_replace = r.func(L"ReplaceSpineData", 3);
        s.sp_setup_pose = r.func(L"SetToSetupPose", 0);
        s.sp_slots_setup = r.func(L"SetSlotsToSetupPose", 0);
        s.sp_update_world = r.func(L"UpdateWorldTransform", 0);
        s.sp_set_animation = r.func(L"SetAnimation", 3);
        s.sp_get_current = r.func(L"GetCurrent", 1);
        c.take(r);
        Resolver opt("SpineSkeletonAnimationExComponent(optional)", cls(L"/Script/SpineExtensionPlugin.SpineSkeletonAnimationExComponent"));
        s.sp_force_tick = opt.func(L"ForceTick", 1);
        if (!opt.ok()) g_optional_missing = opt.failure();
    }
    {
        Resolver r("TrackEntry", cls(L"/Script/SpinePlugin.TrackEntry"));
        s.te_animation_name = r.func(L"getAnimationName", 0);
        s.te_track_time = r.func(L"GetTrackTime", 0);
        s.te_loop = r.func(L"GetLoop", 0);
        c.take(r);
    }
    {
        Resolver r("BaseInventory", cls(L"/Script/Zenith.BaseInventory"));
        s.binv_items = r.prop(L"Items", Kind::Array);
        c.take(r);
        Resolver d("InventoryBaseItemData", cls(L"/Script/Zenith.InventoryBaseItemData"));
        s.item_row_name = d.prop(L"RowName", Kind::Name, 8);
        c.take(d);
        Resolver i("InventoryComponent", cls(L"/Script/Zenith.InventoryComponent"));
        s.inv_currencies = i.prop(L"Currencies", Kind::Map);
        s.inv_sub[0] = i.prop(L"ItemAptitudeInventory", Kind::Object);
        s.inv_sub[1] = i.prop(L"ItemParameterInventory", Kind::Object);
        s.inv_sub[2] = i.prop(L"ItemPassiveInventory", Kind::Object);
        s.inv_sub[3] = i.prop(L"ItemSpiritInventory", Kind::Object);
        s.inv_add_currency = i.func(L"AddCurrency", 2);
        c.take(i);
        Resolver p("ParameterPlayerComponent", cls(L"/Script/Zenith.ParameterPlayerComponent"));
        s.pp_add_experience = p.func(L"AddExperiencePoints", 2);
        c.take(p);
        Resolver w("World", cls(L"/Script/Engine.World"));
        s.world_game_mode = w.prop(L"AuthorityGameMode", Kind::Object);
        c.take(w);
        Resolver g("GameModeZenithBase", cls(L"/Script/Zenith.GameModeZenithBase"));
        s.gm_level_table = g.prop(L"ParameterLevelTable", Kind::Object);
        c.take(g);
    }
    {
        Resolver ch("Character", cls(L"/Script/Engine.Character"));
        s.char_movement = ch.prop(L"CharacterMovement", Kind::Object);
        c.take(ch);
        Resolver cm("CharacterMovementComponent", cls(L"/Script/Engine.CharacterMovementComponent"));
        s.cm_mode = cm.prop(L"MovementMode", Kind::Any, 1);
        s.cm_disable = cm.func(L"DisableMovement", 0);
        s.cm_set_mode = cm.func(L"SetMovementMode", 2);
        s.mc_stop = cm.func(L"StopMovementImmediately", 0);
        c.take(cm);
        Resolver pr("PrimitiveComponent", cls(L"/Script/Engine.PrimitiveComponent"));
        s.prim_ignore_actor = pr.func(L"IgnoreActorWhenMoving", 2);
        s.prim_set_overlaps = pr.func(L"SetGenerateOverlapEvents", 1);
        c.take(pr);
        Resolver a("Actor", cls(L"/Script/Engine.Actor"));
        s.actor_components_by_class = a.func(L"K2_GetComponentsByClass", 1);
        s.actor_component_by_class = a.func(L"GetComponentByClass", 1);
        c.take(a);
        s.primitive_class = ObjHandle::of(reflect::find_object(L"/Script/Engine.PrimitiveComponent"));
        Resolver ac("AutoCollectComponent", cls(L"/Script/Zenith.AutoCollectComponent"));
        s.autocollect_sphere = ac.prop(L"SphereComponent", Kind::Object);
        c.take(ac);
        s.autocollect_class = ObjHandle::of(reflect::find_object(L"/Script/Zenith.AutoCollectComponent"));
        Resolver t("TriggerBase", cls(L"/Script/Engine.TriggerBase"));
        s.trigger_collision = t.prop(L"CollisionComponent", Kind::Object);
        c.take(t);
        Resolver b("BoxComponent", cls(L"/Script/Engine.BoxComponent"));
        s.box_extent = b.prop(L"BoxExtent", Kind::Struct, 12);
        c.take(b);
        Resolver sc("SceneComponent", cls(L"/Script/Engine.SceneComponent"));
        s.scene_rel_scale = sc.prop(L"RelativeScale3D", Kind::Struct, 12);
        c.take(sc);
    }
    {
        Resolver r("GameplayStatics", cls(L"/Script/Engine.GameplayStatics"));
        s.gs_begin_spawn = r.func(L"BeginDeferredActorSpawnFromClass", 5);
        s.gs_finish_spawn = r.func(L"FinishSpawningActor", 2);
        c.take(r);
        s.gs_cdo = ObjHandle::of(reflect::find_object(L"/Script/Engine.Default__GameplayStatics"));
        if (s.gs_cdo.empty() && c.why.empty()) c.why = "GameplayStatics CDO missing";
    }
    s.engine_camera_manager_class = ObjHandle::of(reflect::find_object(L"/Script/Engine.PlayerCameraManager"));
    s.zenith_character_class = ObjHandle::of(reflect::find_object(L"/Script/Zenith.ZenithCharacter"));
    s.zenith_pc_class = ObjHandle::of(reflect::find_object(L"/Script/Zenith.ZenithPlayerController"));
    if (c.why.empty() && (s.engine_camera_manager_class.empty() || s.zenith_character_class.empty() || s.zenith_pc_class.empty()))
        c.why = "engine / Zenith classes missing";

    g_script_failure = c.why;
    if (!c.why.empty()) {
        log::error("core schema (script) unresolved: {}", c.why);
        return false;
    }
    if (!g_optional_missing.empty()) log::warn("core schema optional: {}", g_optional_missing);
    g_script = s;
    log::info("core schema (script) resolved");
    return true;
}

bool resolve_blueprint() {
    Blueprint b;
    Collect c;
    auto* pc = reflect::load_object(kControllerClass);
    auto* pawn = reflect::load_object(kPawnClass);
    b.pc_class = ObjHandle::of(pc);
    b.pawn_class = ObjHandle::of(pawn);
    if (!pc) c.why = "PC_Base_C not loaded";
    else if (!pawn) c.why = "BP_p0000_Lily_C not loaded";
    else {
        Resolver r("BP_p0000_Lily_C", static_cast<ue::UStruct*>(pawn));
        b.lily_spine = r.prop(L"SpineSkeletonAnimationEx", Kind::Object);
        c.take(r);
        Resolver opt("BP_p0000_Lily_C(optional)", static_cast<ue::UStruct*>(pawn));
        b.lily_fake_light = opt.prop(L"FakeLight", Kind::Object);
        if (!opt.ok()) log::warn("core schema optional: {}", opt.failure());
    }
    const auto table = [&](ObjHandle& h, const wchar_t* path) {
        auto* t = reflect::load_object(path);
        h = ObjHandle::of(t);
        if (!t && c.why.empty()) c.why = "data table missing: " + log::utf8(path);
    };
    table(b.table_aptitude, L"/Game/_Zenith/Gameplay/Data/DT_Items_Aptitudes.DT_Items_Aptitudes");
    table(b.table_spirit, L"/Game/_Zenith/Gameplay/Data/DT_Items_Spirits.DT_Items_Spirits");
    table(b.table_passive, L"/Game/_Zenith/Gameplay/Data/DT_Items_Passives.DT_Items_Passives");
    table(b.table_parameter, L"/Game/_Zenith/Gameplay/Data/DT_Items_Parameters.DT_Items_Parameters");
    g_bp_failure = c.why;
    if (!c.why.empty()) {
        log::warn("core schema (blueprint) unresolved: {}", c.why);
        return false;
    }
    g_bp = b;
    log::info("core schema (blueprint) resolved");
    return true;
}

} // namespace

const Script* script() {
    if (g_script) return &*g_script;
    if (g_script_tried) return nullptr; // /Script classes do not appear later; a failure is final for this build
    if (lifecycle::state() == lifecycle::State::Boot) return nullptr;
    g_script_tried = true;
    return resolve_script() ? &*g_script : nullptr;
}

const Blueprint* blueprint() {
    if (g_bp && !g_bp->pc_class.valid()) g_bp.reset();
    if (g_bp) return &*g_bp;
    if (lifecycle::state() != lifecycle::State::Settled) return nullptr;
    const auto now = clock::now_us();
    if (g_bp_generation == lifecycle::generation() && now - g_bp_tried_us < 2'000'000) return nullptr;
    g_bp_generation = lifecycle::generation();
    g_bp_tried_us = now;
    return resolve_blueprint() ? &*g_bp : nullptr;
}

std::string status() {
    std::string s = "script: " + (g_script ? std::string("ok") : g_script_failure);
    s += "; blueprint: " + (g_bp ? std::string("ok") : g_bp_failure);
    if (!g_optional_missing.empty()) s += "; optional missing: " + g_optional_missing;
    return s;
}

void on_teardown() {
    g_bp.reset();
    g_bp_failure = "not resolved yet";
    g_bp_generation = ~0ull;
}

} // namespace elmp::core::schema
