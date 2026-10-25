#pragma once

// PX cold storage (C12, F1.14): Saved/ELMP/ELMP/party_<saveId>.json (schema 1, atomic write + .bak):
//   characters { "<slot>": { active, input devices|script, team friendly|enemy, skin 1..6,
//                             equipment { spirits [{set, command, id}], summonSet, passives [ids] }, unavailable? } }
// Owned items / levels are not stored (shared progress comes from P1); HP / MP / prayers are not stored.
//   - capture: on join, leave (active = false, equipment kept), side / skin change, PX menu close, bench rest;
//   - restore: when save_scope binds, every active character re-joins its slot, then equipment is applied
//     exactly through the vanilla entries, filtered by what P1 owns (missing items: stored list kept, marked
//     unavailable and reported; it comes back when P1 owns them again), then the skin;
//   - migration: a v1 party_<saveId>.profile.lua is read once (read-only, kept). Unreadable PX data (JSON or
//     legacy) is never replaced silently: nothing is loaded, the reason is shown (`elmp storage`, manager) and
//     only an explicit "New PX data" starts this save without a party (§7.13.3).
// Joining is refused while the save is not identified or waiting for a choice (a party must have a home).

#include <cstdint>
#include <string>

namespace elmp::party::storage {

void capture(std::int64_t slot, bool active, bool edited = false);
std::string join_allowed(); // empty = ok

struct View {
    std::string id, message;
    bool blocked = false, can_reuse = false;
};
View view();
std::string choose(bool new_data); // New PX data / Reuse previous (manager confirmation, `elmp storage new|reuse`)
std::string describe();
void install();

} // namespace elmp::party::storage
