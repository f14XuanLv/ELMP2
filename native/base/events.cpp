#include "base/events.h"

#include "base/clock.h"

#include <algorithm>

namespace elmp::events {

const char* name(Type t) noexcept {
    switch (t) {
    case Type::None: return "none";
    case Type::WorldTeardown: return "worldTeardown";
    case Type::WorldSettled: return "worldSettled";
    case Type::MapSwitchStart: return "mapSwitchStart";
    case Type::MapReady: return "mapReady";
    case Type::SeatJoin: return "seatJoin";
    case Type::SeatLeave: return "seatLeave";
    case Type::Damaged: return "damaged";
    case Type::Death: return "death";
    case Type::Rested: return "rested";
    case Type::Recalled: return "recalled";
    case Type::Revived: return "revived";
    case Type::PvpHit: return "pvpHit";
    case Type::NetPeerJoin: return "netPeerJoin";
    case Type::NetPeerLeave: return "netPeerLeave";
    }
    return "?";
}

std::uint64_t Bus::push(Type type, std::int32_t slot, std::int64_t a, std::int64_t b) {
    const auto t = clock::now_us();
    std::scoped_lock lock(mutex_);
    const auto seq = next_++;
    ring_[seq % kCapacity] = Event{seq, t, type, slot, a, b};
    return seq;
}

std::size_t Bus::since(std::uint64_t& cursor, std::span<Event> out, std::uint64_t* lost) const {
    std::scoped_lock lock(mutex_);
    const std::uint64_t last = next_ - 1;
    const std::uint64_t oldest = last >= kCapacity ? last - kCapacity + 1 : 1;
    std::uint64_t from = cursor + 1;
    if (lost) *lost = from < oldest ? oldest - from : 0;
    from = std::max(from, oldest);
    std::size_t n = 0;
    for (std::uint64_t s = from; s <= last && n < out.size(); ++s) out[n++] = ring_[s % kCapacity];
    if (n) cursor = out[n - 1].seq;
    else if (from > last) cursor = std::max(cursor, last);
    return n;
}

std::uint64_t Bus::last_seq() const {
    std::scoped_lock lock(mutex_);
    return next_ - 1;
}

std::uint64_t Bus::dropped() const {
    std::scoped_lock lock(mutex_);
    return next_ - 1 > kCapacity ? next_ - 1 - kCapacity : 0;
}

Bus& bus() {
    static Bus b;
    return b;
}

} // namespace elmp::events
