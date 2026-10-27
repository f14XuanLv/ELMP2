#pragma once

// Command channel (ADR 0006 / 需求树 §7.21): the decision of every replicated actor is the start of a
// command in its UCommandQueueComponent (enemy behaviour tree → ExecuteActionAsync → AddNewImmediateCommand;
// lily input → TryAddCommands; spirits derive from the summoner's command). The host captures the start
// edge with a native detour of the single command step (0xCB4F00, first processing of a UCommand whose
// CurrentResult is still NONE); the client replays it on the presented actor through the vanilla
// AddNewImmediateCommand(dir, cmd, class), and the vanilla execution layer (State / Ability / Move / FX /
// Sound timelines, Spine, abilities, spirits) produces the presentation.

#include "engine/ue.h"

#include <cstdint>
#include <string>

namespace elmp::coop::commands {

struct Start {
    ue::UObject* owner = nullptr;  // the actor owning the CommandQueueComponent
    ue::UObject* cls = nullptr;    // the CommandAction class
    std::string path;              // class path (cached per class)
    std::uint8_t dir = 0, cmd = 0; // EDirectionInputTypes / ECommandInputTypes
};

// Host: install (once) and enable capture; `sink` receives every command start on the game thread.
bool host_enable(void (*sink)(const Start&));
void host_disable();

// Client: replay on `owner` (false + reason on any validation failure). The class must be a child of
// /Script/Zenith.CommandAction; cmd must be a combat / movement type (≤ HEAL).
bool replay(ue::UObject* owner, const std::string& cls_path, std::uint8_t dir, std::uint8_t cmd, std::string* why);

// Client (lily copies): the copy's input → command generation (UCommandQueueComponent::TryAddCommands from
// the pawn tick, 0xCC2520) is suppressed — its commands come only from the host (replay above); its input
// still drives locomotion and the hold semantics of running commands.
bool suppress_input_commands(ue::UObject* pawn, bool on);
void clear_suppressed();

// Dev: returns the command starts recorded since the previous call (owner name contains `filter`), then
// (re)starts recording when `on`.
std::string observe(bool on, const std::string& filter);

std::string describe();

} // namespace elmp::coop::commands
