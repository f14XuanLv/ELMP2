#include "party/config.h"

#include "base/settings.h"

namespace elmp::party::config {

void define() {
    using settings::Spec;
    settings::define(Spec{.key = "mapRule", .def = 3.0, .min = 1, .max = 3, .help = "1 anyone at a door moves everyone, 2 everyone at the door, 3 only P1 (default)"});
    settings::define(Spec{.key = "equipAnywhere", .def = false, .help = "PX may change spirits / relics away from a bench"});
    settings::define(Spec{.key = "muteInput", .def = false, .persist = false, .help = "silence every physical keyboard / pad for all players (not saved)"});
    settings::define(Spec{.key = "recall", .def = true, .help = "bring PX back to P1 when they fall out or stay too far"});
    settings::define(Spec{.key = "recallDistance", .def = 2500.0, .min = 300, .max = 100000, .help = "PX farther than this from P1 (UU) ..."});
    settings::define(Spec{.key = "recallDelayMs", .def = 2000.0, .min = 0, .max = 60000, .help = "... for this long are recalled"});
    settings::define(Spec{.key = "recallFallDepth", .def = 2000.0, .min = 300, .max = 100000, .help = "PX this far below P1 are recalled at once"});
    settings::define(Spec{.key = "cameraFraming", .def = true, .help = "widen the shared camera when PX are present"});
    settings::define(Spec{.key = "cameraSideMargin", .def = 350.0, .min = 0, .max = 5000, .help = "extra framing left / right (UU)"});
    settings::define(Spec{.key = "cameraTopMargin", .def = 150.0, .min = 0, .max = 5000, .help = "extra framing above (UU)"});
    settings::define(Spec{.key = "cameraBottomMargin", .def = 250.0, .min = 0, .max = 5000, .help = "extra framing below (UU)"});
    settings::define(Spec{.key = "cameraMaxDistanceScale", .def = 1.5, .min = 1, .max = 4, .help = "MaxDistance multiplier with PX"});
    settings::define(Spec{.key = "pvpDamage", .def = 0.1, .min = 0, .max = 1, .help = "damage multiplier between opposing players"});
    settings::define(Spec{.key = "pvpKnockback", .def = 0.0, .min = 0, .max = 3, .help = "0 vanilla toughness, 1 light, 2 medium, 3 heavy"});
}

int map_rule() { return static_cast<int>(settings::get_number("mapRule")); }
bool equip_anywhere() { return settings::get_bool("equipAnywhere"); }
bool mute_input() { return settings::get_bool("muteInput"); }
bool recall() { return settings::get_bool("recall"); }
double recall_distance() { return settings::get_number("recallDistance"); }
double recall_fall_depth() { return settings::get_number("recallFallDepth"); }
double recall_delay_ms() { return settings::get_number("recallDelayMs"); }

} // namespace elmp::party::config
