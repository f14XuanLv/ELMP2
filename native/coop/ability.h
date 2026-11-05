#pragma once

// Action channel (§7.5 host/events act, v1 net-facts「动作通道」): enemy ability launches on the host are
// captured by a native detour of UAbilityComponent::LaunchAbilityFromDescription (pre, FAbilityTimeline
// calls it directly — no ProcessEvent) and replayed on the client through the same vanilla UFunction.
// Only root descriptions cross the wire: a replayed native container owns its descendants and FX.
// Whitelisted fields only: AbilityClass (must be a child of /Script/Zenith.Ability), Offset, Spawn/Attach
// bone (+ custom names), follow-rotation flags, fire-and-forget, and the original AbilityParams.

#include "net/protocol.h"

#include "engine/ue.h"

#include <functional>
#include <string>

namespace elmp::coop::ability {

struct Launch {
    ue::UObject* owner = nullptr; // the actor whose AbilityComponent launched
    ue::UObject* spawned = nullptr; // the launched AAbility (pooled; may be null when the launch failed)
    std::uint64_t activation = 0;   // native Launch entry identity / capture order, not UObject lifetime
    std::string cls;              // class path
    net::proto::Act act;          // net_id / ability / custom names filled by the caller (name ids)
    std::string custom_spawn, custom_attach;
};

// Host: install (once) and enable capture; `sink` receives every launch on the game thread.
bool host_enable(void (*sink)(const Launch&));
void host_disable();

// Client: replay on `owner` (enemy / lily copy / spirit copy; false + reason in *why on any validation
// failure); *spawned_out = the replayed ability (ActStop target).
bool replay(ue::UObject* owner, const net::proto::Act& act, const std::string& cls, const std::string& custom_spawn, const std::string& custom_attach,
            std::string* why, ue::UObject** spawned_out = nullptr, std::uint64_t* activation_out = nullptr);

// Exact native activation identity. Pool reuse replaces it even if ObjHandle is unchanged.
std::uint64_t activation(ue::UObject* actor);
std::uint64_t next_capture_order();
bool native_writer(ue::UObject* actor); // only a captured/replayed root or its original descendants

struct Identity {
    std::uint64_t token = 0, root = 0;
    net::proto::AbilityPath path;
    std::string cls;
    bool supported = false, active = false;
};
// Copy at native Stop entry; unsupported descendants still return an identity, never a guessed path.
bool identity(ue::UObject* actor, Identity& out);
enum class ChildState { Missing, Active, Ended, Conflict };
ChildState child_state(std::uint64_t root, const net::proto::AbilityPath& path, const std::string& cls);
ue::UObject* find_child(std::uint64_t root, const net::proto::AbilityPath& path, const std::string& cls, std::uint64_t* token_out = nullptr);
bool has_active_family(std::uint64_t root);
// Called after the original native Stop returns, using its entry identity. Reentrant new activation
// on the same UObject is untouched; old child tombstones survive until the family is retired.
void stopped(ue::UObject* actor, const Identity& before);
std::string fault(); // first network lifecycle fault; owning session leaves on its next game tick
void reset_fault(); // explicit new session / completed teardown only, never hides a pending map fault

// Keep native OnStop / asynchronous OnTick descendants inside their original writer. No permission
// gate is changed. Restoration is stack based so native reentrant Launch/Stop cannot leak a scope.
class NativeScope {
public:
    explicit NativeScope(ue::UObject* actor);
    ~NativeScope();
    NativeScope(const NativeScope&) = delete;
    NativeScope& operator=(const NativeScope&) = delete;
private:
    ue::UObject* previous_actor_ = nullptr;
    std::uint64_t previous_token_ = 0;
};

// Stop client-owned live abilities through the original Stop while cleanup scopes still exist,
// then discard identities. Root/session counters never rewind; weak invalidation handles World GC.
void client_reset();

std::string describe();

} // namespace elmp::coop::ability
