#pragma once

// Profiles (M4, FR-CORE-2): one player's progress as plain data. Every write goes through a vanilla entry
// (v1 core-facts「档案」):
//   aptitudes    DT_Items_Aptitudes rows      InventoryComponent:AddItem / aptitude inventory RemoveItem
//   parameters   DT_Items_Parameters rows (repeats)   AddItem / RemoveItem + AddMaxHPBonusPercentage(0)
//   spiritItems  DT_Items_Spirits rows (owned)  AddItem / spirit inventory RemoveItem
//   spiritLevels {id: level} or "max"         ItemSpiritInventory:SetSpiritLevel (max = rows of SpiritLevelTable)
//   spirits      [{set, command, id}] + summonSet   SpiritEquipComponent:EquipSpirit / UnequipSpirit / SwitchSummonSet
//   passives     {equipped, owned, slots}     PassiveEquipComponent:EquipPassive / UnequipPassive / SetSlotCount
//   stats        {level, experience, heal{maxCountLevel, countLevel, powerLevel}}
// Apply modes: incremental (default; only adds), exact (present fields match exactly), reset (exact on top
// of BASELINE = a new game). Fields fail independently and are reported. Order: aptitudes and parameters
// before spirits (level changes re-equip), passives after slots, stats last. The core never writes P1.

#include "engine/ue.h"

#include "base/json.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace elmp::core::profile {

struct SpiritSlot {
    int set = 0, command = 0;
    std::string id;
    bool operator==(const SpiritSlot&) const = default;
};
struct Passives {
    std::optional<std::vector<std::string>> equipped, owned;
    std::optional<int> slots;
};
struct Heal {
    std::optional<int> max_count_level, count_level, power_level;
};
struct Stats {
    std::optional<int> level, experience;
    std::optional<Heal> heal;
};
struct Profile {
    std::optional<std::vector<std::string>> aptitudes;
    std::optional<std::vector<std::string>> parameters;
    std::optional<std::vector<std::string>> spirit_items; // owned spirits (DT_Items_Spirits rows), not equipment
    std::optional<std::map<std::string, int>> spirit_levels; // ignored when spirit_levels_max
    bool spirit_levels_max = false;
    std::optional<std::vector<SpiritSlot>> spirits;
    std::optional<int> summon_set;
    std::optional<Passives> passives;
    std::optional<Stats> stats;
};

enum class Preset { Baseline, Default, Full, Max };
std::optional<Preset> preset_named(std::string_view name);
Profile preset(Preset p);
Profile fresh(const Profile& p); // overlay on BASELINE (nested passives / stats merge per sub-field)

json::Value to_json(const Profile& p);
bool from_json(const json::Value& v, Profile& out, std::string* err);

struct FieldResult {
    std::string field;
    bool ok = true;
    std::string detail; // counts or the error
};
struct Report {
    std::vector<FieldResult> fields;
    std::string refused; // whole apply refused (non-ELMP controller, schema)
    bool ok() const;
    std::string text() const;
};

std::optional<Profile> read(ue::UObject* pc, std::string* err); // any controller incl. P1 (read-only)
Report apply(ue::UObject* pc, const Profile& p, bool exact);
Report reset(ue::UObject* pc, const Profile& p);

} // namespace elmp::core::profile
