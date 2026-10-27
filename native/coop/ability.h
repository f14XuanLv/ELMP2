#pragma once

// Action channel (§7.5 host/events act, v1 net-facts「动作通道」): enemy ability launches on the host are
// captured by a native detour of UAbilityComponent::LaunchAbilityFromDescription (pre, FAbilityTimeline
// calls it directly — no ProcessEvent) and replayed on the client through the same vanilla UFunction.
// Whitelisted fields only: AbilityClass (must be a child of /Script/Zenith.Ability), Offset, Spawn/Attach
// bone (+ custom names), follow-rotation flags, fire-and-forget. AbilityParameters are not replicated.

#include "net/protocol.h"

#include "engine/ue.h"

#include <functional>
#include <string>

namespace elmp::coop::ability {

struct Launch {
    ue::UObject* owner = nullptr; // the actor whose AbilityComponent launched
    ue::UObject* spawned = nullptr; // the launched AAbility (pooled; may be null when the launch failed)
    std::string cls;              // class path
    net::proto::Act act;          // net_id / ability / custom names filled by the caller (name ids)
    std::string custom_spawn, custom_attach;
};

// Host: install (once) and enable capture; `sink` receives every launch on the game thread.
bool host_enable(void (*sink)(const Launch&));
void host_disable();

// Client: replay on `owner` (enemy / lily copy / spirit copy; false + reason in *why on any validation
// failure); *spawned_out = the replayed ability (ActStop target).
ue::UClass* class_of(const std::string& cls_path, bool* loaded_now); // cached path → class
bool replay(ue::UObject* owner, const net::proto::Act& act, const std::string& cls, const std::string& custom_spawn, const std::string& custom_attach,
            std::string* why, ue::UObject** spawned_out = nullptr);

std::string describe();

} // namespace elmp::coop::ability
