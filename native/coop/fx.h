#pragma once

// Presentation channels besides abilities (P6, GOAL §5b): FX, sounds and ability ends of replicated actors.
// Host: native detours (data/binary/EL.json) report on the game thread
//   - UBaseFXComponent::SpawnFX implementation (every FX of a character goes through it: FX timelines,
//     Spine anim notifies, ability visual cues, blueprints) → sink with the owner actor and the arguments;
//   - UParticleSystemComponent::DeactivateSystem → sink with the component (the host maps it to an FX seq);
//   - FSoundTimeline::Tick → one sink call per entry whose From the timeline crosses this tick;
//   - AAbility::Stop → sink with the ability and the reason.
// Client: replays through the same vanilla UFunctions; while seated, SpawnFX on a guard-marked (presented)
// owner that is not a replay is suppressed (the host's FX arrive instead — no doubles).

#include "net/protocol.h"

#include "engine/ue.h"

#include <cstdint>
#include <string>

namespace elmp::coop::fx {

struct Spawn {
    ue::UObject* owner = nullptr; // the FX component's actor
    ue::UObject* ps = nullptr;    // UParticleSystem
    ue::UObject* psc = nullptr;   // result
    net::proto::Fx msg;           // bone / xf / flags / sort / pool filled; kind / id / ps / custom bone by the caller
    std::string custom_bone;      // "None" when unset
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
bool host_enable(const Sinks& s);
void host_disable();

// Client: suppression of local SpawnFX on presented owners while seated.
bool client_enable();
void client_disable();
void set_block_local(bool on); // Dev comparison (default on)
// P8 判决性实验：`elmp set fxLocal 1` = 客户端不重放 Host 的 FX、也不抑制本地生成，
// 让表现由客户端自己的模拟产生（敌人域的删除测试）；0 = P6 行为（抑制 + 重放）。
bool local_mode();

// Client replays (false on failure, reason in *why).
ue::UObject* replay_spawn(ue::UObject* actor, const net::proto::Fx& m, const std::string& ps_path, const std::string& custom_bone, std::string* why);
// flags bit3: FX captured at UGameplayStatics::SpawnEmitterAttached (Spine anim notifies / blueprints).
// xf = [0..2] rotator, [4..6] location (in the attach component's space, EAttachLocation in `sort`),
// [7..9] scale; `m.bone` unused and the custom name is the attach component's FName.
ue::UObject* replay_emitter(ue::UObject* actor, const net::proto::Fx& m, const std::string& ps_path, const std::string& attach_comp, std::string* why);
bool deactivate(ue::UObject* psc);
bool play_sound(ue::UObject* actor, const std::string& event_path, bool is2d, std::string* why);
bool stop_ability(ue::UObject* ability, std::uint8_t reason);

std::string path_of(ue::UObject* asset); // cached per object (weak handle checked)
std::string describe();

} // namespace elmp::coop::fx
