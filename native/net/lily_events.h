#pragma once
#include <array>
#include <cstdint>
namespace elmp::net::snap {
struct EventAnimation {
    std::uint32_t generation=0;
    std::uint16_t animation=0;
    std::uint8_t flags=0; // loop, hold
    float time=0,start=0,end=0,blend=0;
    bool operator==(const EventAnimation&) const=default;
};
struct LilyEvents {
    std::array<EventAnimation,4> layers{}; // original DEATH / EVENT_LOCOMOTION / EVENT_KEEP / EVENT
    bool operator==(const LilyEvents&) const=default;
};
}
