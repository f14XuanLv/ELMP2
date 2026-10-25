#pragma once

// LilyCore schema (§4): every property / UFunction the core uses, resolved by name once and type-checked.
// Two groups:
//   - script(): /Script classes (engine, Zenith, Spine). Resolved on first use after the engine is up.
//   - blueprint(): blueprint classes and data tables (PC_Base_C, BP_p0000_Lily_C, DT_Items_*). They only
//     exist after a save is loaded, so a failure is not cached: it is retried on the next request once the
//     world generation changed or 2 s passed (kb: blueprint classes load after the save).
// Any missing field makes its group unusable → every core capability that needs it fails closed and
// `elmp core` prints the exact field.

#include "engine/handle.h"
#include "engine/reflect.h"

#include <array>
#include <string>

namespace elmp::core::schema {

using reflect::FuncRef;
using reflect::PropRef;

struct Script {
    // Actor / components
    PropRef actor_tags, actor_root, actor_destroying;
    FuncRef actor_destroy, actor_disable_input, actor_location, actor_teleport;
    PropRef scene_rel_location;
    FuncRef prim_set_material;
    // Pawn / controllers
    PropRef pawn_controller, controller_pawn, pc_player, pc_camera_class;
    FuncRef controller_possess, controller_unpossess;
    PropRef zpc_inventory, zpc_param, zpc_spirit_equip, zpc_heal, zpc_passive;
    FuncRef zpc_fully_restore, zpc_aptitude_unlocked;
    PropRef zpc_unlocked; // TArray<EAptitudeType>, read directly by the progress snapshot
    // ZenithCharacter
    PropRef zc_input, zc_hp, zc_mp, zc_faction, zc_command_queue, zc_locomotion;
    FuncRef loco_facing_right;
    PropRef zic_pawn_owner, zic_snapshot;
    PropRef hp_curr, hp_max;
    FuncRef hp_alive;
    PropRef mp_curr, mp_max;
    FuncRef mp_add, mp_reset;
    PropRef heal_max_count_level;
    FuncRef heal_remaining, heal_max_count, heal_count_level, heal_power_level, heal_set_count_level, heal_set_power_level, heal_fully_restore;
    FuncRef cq_has_command;
    PropRef faction_value;
    // Inventory
    FuncRef inv_add_item, inv_aptitudes, inv_spirits, inv_passives, inv_parameters;
    FuncRef binv_count, binv_handle_at, binv_remove;
    PropRef spirit_levels;
    FuncRef spirit_set_level, spirit_get_level;
    // Equipment
    PropRef se_sets, set_spirits, spirit_id;
    FuncRef se_equip, se_unequip, se_unequip_all, se_switch, se_current, se_reequip;
    PropRef pe_equipped, passive_id;
    FuncRef pe_equip, pe_unequip, pe_is_equipped, pe_slot_count, pe_set_slot_count;
    // Parameters
    PropRef pp_experience;
    FuncRef pp_level, pp_set_level, pp_need_exp, pp_add_max_hp_bonus;
    // Spine
    PropRef sk_atlas;
    FuncRef sp_replace, sp_setup_pose, sp_slots_setup, sp_update_world, sp_set_animation, sp_get_current;
    FuncRef sp_force_tick; // optional (logged when missing)
    FuncRef te_animation_name, te_track_time, te_loop;
    // Progress snapshots (direct reads, no ProcessEvent)
    PropRef binv_items, item_row_name, inv_currencies;
    std::array<PropRef, 4> inv_sub; // aptitude, parameter, passive, spirit sub-inventories (direct pointers)
    FuncRef inv_add_currency, pp_add_experience;
    PropRef world_game_mode, gm_level_table;
    // Movement / placement / gates
    PropRef char_movement, cm_mode;
    FuncRef cm_disable, cm_set_mode, mc_stop;
    FuncRef prim_ignore_actor, actor_components_by_class;
    ObjHandle primitive_class;
    PropRef trigger_collision, box_extent, scene_rel_scale;
    // Spawning (GameplayStatics CDO)
    FuncRef gs_begin_spawn, gs_finish_spawn;
    ObjHandle gs_cdo;
    ObjHandle engine_camera_manager_class; // /Script/Engine.PlayerCameraManager
    ObjHandle zenith_character_class, zenith_pc_class;
};

struct Blueprint {
    ObjHandle pc_class, pawn_class;
    PropRef lily_spine, lily_fake_light;
    ObjHandle table_aptitude, table_spirit, table_passive, table_parameter;
};

const Script* script();       // nullptr when unresolved (see status())
const Blueprint* blueprint(); // nullptr when unresolved; retried later
std::string status();         // "ok" or the first failing field per group
void on_teardown();           // drops the blueprint group so the next world re-resolves it

// Asset paths (v1 core/assets; same as GameModeZenith's PlayerControllerClass / DefaultPawnClass).
inline constexpr const wchar_t* kControllerClass = L"/Game/_Zenith/Core/Controllers/PC_Base.PC_Base_C";
inline constexpr const wchar_t* kPawnClass = L"/Game/_Zenith/Characters/p0000_Lily/BP_p0000_Lily.BP_p0000_Lily_C";

} // namespace elmp::core::schema
