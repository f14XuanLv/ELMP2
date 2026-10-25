#pragma once

// Vanilla interaction for PX (F1.16 / §7.1.2; kb party-facts「P1f 交互」). The seat's own InteractComponent
// already selects the nearest interactable and activates it for the seat's controller (sphere overlap,
// filtered by the `interact` / `pickup` / `rest` gates). What a PX lacks is the input: with a dedicated
// Interact key bound for P1's device (EInteractableInputType NONE) the interactable only listens to the
// UE action "Interact" on its own InputComponent, which only P1's local player feeds. On a PX INTERACT
// press edge this module hands the press to that same native action handler (manifest
// `InteractablePressInteract`) for the interactable whose interacting controller is the seat's — IsPaused /
// IsInteractable / OnInteract / OnMarkCleared stay vanilla. UP-type interactables already work through the
// command queue (seat presses Up). Event-driven: a few reflection calls per press, nothing per frame.

#include <cstdint>
#include <string>

namespace elmp::party::interact {

// Delivers one press; returns what happened ("" = nothing to interact with / not applicable).
std::string press(std::int64_t slot);
std::string status();

} // namespace elmp::party::interact
