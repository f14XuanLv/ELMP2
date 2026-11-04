#pragma once

// Presentation channels besides abilities (P6, GOAL §5b): FX, sounds and ability ends of replicated actors.
// Host: native detours (data/binary/EL.json) report on the game thread
//   - UBaseFXComponent::SpawnFX and SpawnEmitterAttached → owner and spawn arguments;
//   - UParticleSystemComponent::DeactivateSystem → sink with the component (the host maps it to an FX seq);
//   - PlayEventAtLocation → owner and event (other FMOD paths remain separate coverage gaps);
//   - AAbility::Stop → sink with the ability and the reason.
// Client: replays through the same vanilla UFunctions; while seated, SpawnFX on a guard-marked (presented)
// owner that is not a replay is suppressed (the host's FX arrive instead — no doubles).

#include "net/protocol.h"

#include "engine/ue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace elmp::coop::fx {

struct Spawn {
    ue::UObject* owner = nullptr; // the FX component's actor
    ue::UObject* ps = nullptr;    // UParticleSystem
    ue::UObject* psc = nullptr;   // result
    net::proto::Fx msg;           // bone / xf / flags / sort / pool filled; kind / id / ps / custom bone by the caller
    std::string custom_bone;      // "None" when unset
    std::string attach_component, attach_socket; // emitter identity and socket, independently interned
};
struct Sound {
    ue::UObject* owner = nullptr;
    ue::UObject* event = nullptr; // UFMODEvent
    bool is2d = false;
};
struct Sinks {
    void (*spawn)(const Spawn&) = nullptr;
    void (*deactivate)(ue::UObject* psc) = nullptr;
    void (*sound)(const Sound&) = nullptr;
    void (*ability_stop)(ue::UObject* ability, std::uint8_t reason) = nullptr;
};

// Host: install (once) and route to `s`; host_disable stops reporting (detours stay, pass-through).
// install() can be called at engine init: the detours then only record *persistent* FX (bRegisterAsRunningFX)
// that appear before a session exists, so a host that starts hosting after the map load can still hand them
// to a late joiner (take_early drains that list).
void install();
std::size_t take_early(std::vector<Spawn>& out);
bool active(ue::UObject* psc); // reflected bIsActive; weak validity / native Deactivate provide the other invalidators
bool host_enable(const Sinks& s);
void host_disable();

// Client: suppression of local SpawnFX on presented owners while seated.
bool client_enable();
void client_disable();

// Client replays (false on failure, reason in *why).
ue::UObject* replay_spawn(ue::UObject* actor, const net::proto::Fx& m, const std::string& ps_path, const std::string& custom_bone, std::string* why);
// flags bit3: FX captured at UGameplayStatics::SpawnEmitterAttached (Spine anim notifies / blueprints).
// xf = actual PSC relative rotator/location/scale after vanilla processed EAttachLocation (`sort`).
// Replayed as KeepRelativeOffset at the explicit component/socket. Bone kind uses m.bone + flags bit1.
// Both replay entries use PoolMethod=None: remote lifetimes never borrow a local P1's pooled PSC.
ue::UObject* replay_emitter(ue::UObject* actor, const net::proto::Fx& m, const std::string& ps_path,
                           const std::string& attach_comp, const std::string& socket, std::string* why);
bool deactivate(ue::UObject* psc);
bool retire(ue::UObject* psc); // network-owned only: deactivate now, global phase destroys after native completion
bool play_sound(ue::UObject* actor, const std::string& event_path, bool is2d, std::string* why);
bool stop_ability(ue::UObject* ability, std::uint8_t reason);

std::string path_of(ue::UObject* asset); // cached per object (weak handle checked)
std::string describe();

} // namespace elmp::coop::fx
