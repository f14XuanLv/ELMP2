#pragma once

// Host map slots (§7.5 host/maps, C18, FR-NET-10): a remote seat may stand in another map than host P1. Slot
// 0 = the foreground map (P1's); slot k ≥ 1 = a background instance (world/level_instance) at Y +300000·k.
// Network seats never touch a vanilla door (their travel target is always ignored); every 250 ms a network
// seat with the travel gate open that stands inside a door box is moved: destination = foreground → the
// foreground PlayerStart with the door's tag; otherwise the destination is loaded in the background and the
// seat is placed at the matching PlayerStart of that instance once it is ready. A background map without
// seats is unloaded after a 10 s grace; host P1 switching into a background map unloads it first.
// Snapshot rows are banded by Y (slot k ↔ 300000·k ± 150000); a peer receives the rows of its seat's map,
// translated to that map's local coordinates.

#include "core/player.h"

#include <cstdint>
#include <optional>
#include <string>

namespace elmp::coop::maps {

constexpr float kSlotY = 300000.0f;
constexpr float kBand = 150000.0f;

int slot_of_y(float y);                         // 0 = foreground, k = background slot, -1 = outside every band
int slot_of_seat(std::int64_t seat);            // slot the seat's pawn stands in (-1 none)
std::string map_id(int slot);                   // "" for an unused slot
float offset(int slot);
std::string tag_of_seat(std::int64_t seat);      // PlayerStart tag of the seat's last move into a background map                         // Y offset of the slot (0 for the foreground)
void tick(std::int64_t now_us);                 // host: door polling, readiness, grace unloads
void before_foreground_switch(const std::string& target_map); // host P1 is about to load target_map
std::optional<core::Vec3> entrance_of_seat(std::int64_t seat); // a PlayerStart of the map the seat stands in
bool boss_room(float y);                        // the band containing y holds an uncleared boss room (§7.1.3)
void reset();
std::string describe();

} // namespace elmp::coop::maps
