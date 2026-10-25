#pragma once

// Save scope (C12, F1.14): which PX data belongs to the loaded vanilla save.
//   - identity: a random 32-hex saveId per save "generation", kept in Saved/ELMP/ELMP/save_index.json by save
//     slot name (SaveSubsystem.SaveSlotName, e.g. GameSlot_0) with the SHA-256 of the last 64 known .sav
//     contents (fingerprints); the vanilla save has no stable GUID (v1 party-facts「PX 冷存储」).
//   - bind (once per world, after the save is loaded): LaunchGameIntent NewGame or no entry → new saveId;
//     otherwise the current .sav fingerprint must be a known one, else **blocked** (external replacement,
//     rollback, unreadable file): nothing is loaded until the player explicitly chooses New PX data / Reuse.
//   - registration of saves: the .sav write time is polled at 1 Hz while bound (no reflection); a change that
//     follows a vanilla SaveGame / SaveGameAsync call (UFunction pre hooks, any path incl. the bench's async
//     save) is hashed once stable and added; a change without a save call is logged as external and not added.
//   - DeleteSaveDataAtSlotIndex / DeleteAllSaveData / CopyGameDataToEmptySlot: entries whose .sav no longer
//     exists are dropped (old PX files are never deleted).
//   - first run: a v1 save_index.profile.lua is migrated read-only (§7.13.3: failure blocks, file kept).

#include <cstdint>
#include <string>

namespace elmp::party::save_scope {

enum class State : std::uint8_t { Unbound, Bound, Blocked };
struct Status {
    State state = State::Unbound;
    std::string id, slot_name, reason;
    bool can_reuse = false; // blocked by an unknown fingerprint with a previous saveId
};
Status status();
bool busy(); // a bind attempt is due in this settled world (frame activity: it uses ProcessEvent)

std::string choose(bool new_data); // explicit New PX data / Reuse previous (only while blocked)
std::string force_new();           // storage-level refusal (unreadable PX data): new generation for this slot

using Bound = void (*)(const std::string& id, bool fresh);
void on_bound(Bound fn);
std::string describe();
void install();

} // namespace elmp::party::save_scope
