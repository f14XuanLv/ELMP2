#pragma once

#include <array>
#include <cstdint>

namespace elmp::net::snap {

// Host component output, not Actor world coordinates. Missing data is never a zero transform.
struct Visual {
    std::uint8_t valid = 0; // bit 0 VisualPivot, bit 1 renderer, bit 2 renderer Color, bit 3 DissolveValue, bit 4 root bone scale
    // Relative XYZ, pitch/yaw/roll, scale XYZ.
    std::array<float, 9> pivot{}, renderer{};
    std::array<float, 4> color{};
    float dissolve = 0;
    std::array<float,2> root_scale{1,1};
};

} // namespace elmp::net::snap
