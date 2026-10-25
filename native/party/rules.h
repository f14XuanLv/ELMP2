#pragma once

// Map rules (F1.7, C8, §7.4, §7.13.4), local seats only:
//   3 (default) only P1 travels; PX ignore doors (travel gate closed).
//   1 any local seat entering a door moves everyone: rule 1 grants travel to local seats (2..100) while
//     active; the vanilla door switches the map and transit carries the rest.
//   2 everyone at the door: P1's own collision ignores every door (explicit P1 door hold — the only gate
//     ELMP puts on P1, needed because the vanilla door reacts to P1 alone) except a door whose box holds
//     every local seat alive; then P1 entering / standing in it triggers the vanilla switch.
// Door boxes = TriggerBase.CollisionComponent (BoxComponent, root) location + BoxExtent × RelativeScale3D,
// cached per door handle (doors do not move).

#include <string>

namespace elmp::party::rules {

void tick(); // patrol (250 ms)
std::string describe();
void install();

} // namespace elmp::party::rules
