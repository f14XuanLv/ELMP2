#pragma once

// Event bus (M21): bounded ring (1024), monotonically increasing seq, cursor reads, loss accounting.
// Producers: detours, UFunction hooks, lifecycle; consumers read with their own cursor
// (party / coop / modes / SDK / pilot). Thread-safe; producers never block on consumers.

#include <array>
#include <cstdint>
#include <mutex>
#include <span>

namespace elmp::events {

enum class Type : std::uint16_t {
    None = 0,
    WorldTeardown,
    WorldSettled,
    MapSwitchStart,
    MapReady,
    SeatJoin,
    SeatLeave,
    Damaged,
    Death,
    Rested,
    Recalled,
    Revived,
    PvpHit,
    NetPeerJoin,
    NetPeerLeave,
};

const char* name(Type t) noexcept;

struct Event {
    std::uint64_t seq = 0;
    std::int64_t t_us = 0;
    Type type = Type::None;
    std::int32_t slot = 0;
    std::int64_t a = 0, b = 0;
};

class Bus {
public:
    static constexpr std::size_t kCapacity = 1024;

    std::uint64_t push(Type type, std::int32_t slot = 0, std::int64_t a = 0, std::int64_t b = 0);

    // Copies events with seq > cursor into out (oldest first), advances cursor to the last copied seq.
    // Returns how many were copied; *lost receives how many were overwritten before being read.
    std::size_t since(std::uint64_t& cursor, std::span<Event> out, std::uint64_t* lost = nullptr) const;

    std::uint64_t last_seq() const;
    std::uint64_t dropped() const; // total overwritten-unread is per cursor; this is the ring wrap count

private:
    mutable std::mutex mutex_;
    std::array<Event, kCapacity> ring_{};
    std::uint64_t next_ = 1; // seq of the next push
};

Bus& bus();

} // namespace elmp::events
