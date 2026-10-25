#pragma once

// Shared-camera framing (F1.6, M15). The vanilla AGameplayCamera already frames every registered
// player / ally controller (party-facts「共享镜头」); with PX present the target box gets margins and
// MaxDistance is scaled for the duration of one vanilla Update call. Two manifest detours
// (GameplayCameraUpdate / GameplayCameraTargetBounds, SHA-256 + prefix verified); field offsets come
// from reflection (CurrentSettings.MaxDistance / WorldSpaceLocationOffset.Z, the two controller target
// arrays). The detours never call reflection; with framing off they are a flag test + the original.

#include <cstdint>
#include <string>

namespace elmp::camera {

struct Margins {
    float side = 350.0f, top = 150.0f, bottom = 250.0f, max_distance_scale = 1.5f;
};

// Game thread. Installs the detours on the first enable; returns the refusal reason (fail closed).
std::string configure(bool enabled, const Margins& margins);

struct Status {
    bool enabled = false, hooked = false;
    std::uint64_t widened = 0; // Update calls that ran widened (≥ 2 friendly targets)
    std::string failure;
};
Status status();

} // namespace elmp::camera
