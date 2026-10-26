#pragma once

// Background map instances (§7.3 C18, FR-NET-10; v1 net-facts「后台图」): a vanilla game map is a set of
// sublevel packages `/Game/_Zenith/Maps/<Area>/Map_<Area>_<NN>/Map_<Area>_<NN>_<Part>` that the
// WorldLoaderSubsystem streams into PersistentGameMap with ULevelStreamingDynamic::LoadLevelInstance at zero
// offset. The host loads another map's GEO / DESIGN / GAMEPLAY the same way at a large Y offset
// (slot k → +300000·k) under instance names `ELMP_BG_<mapId>__<Part>`, so map_context can tell its objects
// apart. Native ProcessEvent of the vanilla static function (UE4SS Lua cannot: bool out parameter).
// Unload = SetIsRequestingUnloadAndRemoval(true) on each streaming level.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace elmp::world::level_instance {

struct Part {
    std::string part, package, instance;
};
std::vector<Part> parts(std::string_view map_id); // empty = unsupported map id

// Loads every part of `map_id` at Y offset; empty = ok (streaming started), else the reason (nothing kept).
std::string load(std::string_view map_id, float offset_y);
void unload(std::string_view map_id);
bool loaded(std::string_view map_id);
// Loaded and visible (every part's streaming level reports its level loaded).
bool ready(std::string_view map_id);
// map id whose ELMP_BG_ instance contains `obj` (walks the Outer chain to the ULevel / package); empty = none.
std::string map_of(const void* obj);
std::string describe();

} // namespace elmp::world::level_instance
