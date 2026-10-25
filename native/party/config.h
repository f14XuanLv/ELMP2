#pragma once

// Local-multiplayer settings (F1.17, §7.11, §7.13.1): declared once with type, default and range, persisted
// by base/settings to Saved/ELMP/ELMP/settings.json; `muteInput` is transient. Defaults per
// REQUIREMENTS-TREE §7 / v1 party/config: recall 2500 / 2000 ms / 2000, pvpDamage 0.1, pvpKnockback 0,
// camera margins 350 / 150 / 250 × 1.5, map rule 3, equipAnywhere off.

namespace elmp::party::config {

void define(); // before settings::load

int map_rule();
bool equip_anywhere();
bool mute_input();
double recall_distance();
double recall_fall_depth();
double recall_delay_ms();
bool recall();

} // namespace elmp::party::config
