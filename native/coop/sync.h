#pragma once

// `elmp lan sync [cat]` (P7 restart §3, ADR 0006): comparable JSON lines of what this process actually
// presents, read from the live actors (not from the snapshot rows), so host and client outputs can be diffed
// by tools/sync_diff.py. Keys are role independent:
//   e:<spawn point name>  spawn-point enemy        d:<dyn id>  host-spawned (enemy_dyn) enemy
//   l:<seat>              lily (1 = host P1)       s:<seat>:<class>  visible spirit of a seat
//   x:<object name>       client-local enemy bound to no host row (a leak / extra on the client)
// Categories: ent (entities), world (world objects), fx (abilities / active particles / playing FMOD components per owner; one
// object-array walk — cold path, Dev diagnostics only), cnt (guard + role counters), hash (Saved files).
// Every line is a single-line JSON object with "cat".

#include "engine/ue.h"

#include <functional>
#include <string>
#include <vector>

namespace elmp::coop::sync {

bool wants(const std::string& cat, const char* c); // "" / "all" → every category

struct Ent {
    std::string key;
    ue::UObject* actor = nullptr;
    bool lily = false; // spine component comes from the lily blueprint property
    int slot = 0;      // host map slot (0 foreground); y is written map-local
    float yoff = 0;
};

void header(std::string& out, const char* role, int seat, const std::string& extra_json_fields);
void entity(std::string& out, const Ent& e);
// actor → entity key ("" when the actor is not a replicated entity)
using KeyOf = std::function<std::string(ue::UObject*)>;
void fx(std::string& out, const KeyOf& key_of);
void world_objects(std::string& out); // cat world: interactables / breakables / triggers / chairs + pickups per class
void guard_counters(std::string& out);
void hashes(std::string& out);

// Edge recorder (`elmp lan rec start [frames] | dump`): every frame while armed, the track-0 animation / HP /
// visibility of every entity the role lists; only changes are kept (bounded). The entity list is re-taken
// every 30 frames (spawned copies / spirits). Dev diagnostics: costs a few ProcessEvent calls per entity
// per frame while armed, nothing otherwise.
using EntList = std::function<std::vector<Ent>()>;
std::string rec_start(EntList list, int frames);
void rec_tick(); // from the role's tick (no-op unless armed)
void rec_note(const std::string& key, const std::string& text); // Dev: annotate the recording (events)
std::string rec_dump(); // JSON lines {"cat":"rec","f","k","anim","hp","vis"} + {"cat":"rech",...}

} // namespace elmp::coop::sync
