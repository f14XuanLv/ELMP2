#pragma once

// Entity adapters (§7.5 entities/*, ADR 0003 contract in C++): one adapter per replicated entity kind.
// Host side writes its rows into the frame being captured; client side presents decoded rows (idempotent:
// a row applied twice changes nothing) and may run per-frame presentation work. `reset` = map change /
// session end (client: drop everything presented for the old map; host: forget per-map bookkeeping).
// All six adapters are in coop/entities/; the enemy / lily halves call into coop/host.cpp + coop/client.cpp
// (they share the takeover / seat state there).

#include "net/snapshot.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace elmp::coop {

struct HostCtx {
    net::snap::Frame& frame;
    std::int64_t now_us;
    std::uint16_t (*name)(std::string_view); // intern + NameDef to every peer
};

struct ClientCtx {
    const net::snap::Frame& frame;
    const std::string* (*text)(std::uint16_t); // nullptr = name not known yet (row skipped, retried)
};

class IEntityAdapter {
public:
    virtual ~IEntityAdapter() = default;
    virtual const char* name() const = 0;
    virtual void write_host(HostCtx&) {}
    virtual void apply_client(const ClientCtx&) {}
    virtual void tick_client(std::int64_t /*now_us*/) {}
    virtual void reset() {}
    virtual std::string describe() const { return {}; }
};

std::span<IEntityAdapter* const> adapters();

namespace entities {
IEntityAdapter& lily();
IEntityAdapter& enemy();
IEntityAdapter& worldobj();
IEntityAdapter& story();
IEntityAdapter& pickup();
IEntityAdapter& enemy_dyn();
} // namespace entities

} // namespace elmp::coop
