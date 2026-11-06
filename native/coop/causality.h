#pragma once

#include "engine/ue.h"
#include "net/protocol.h"
#include <string>
#include <string_view>
#include <cstdint>

namespace elmp::coop::ability { struct Identity; }
namespace elmp::coop::causality {
bool enable();
void reset();
struct HostApi {
    std::uint16_t (*name)(std::string_view) = nullptr;
    bool (*place)(std::uint16_t seat, std::uint16_t& map, float& offset) = nullptr;
    bool (*reference)(ue::UObject* actor, net::proto::Cause& fact, std::uint16_t map) = nullptr;
};
struct ClientApi {
    const std::string* (*text)(std::uint16_t) = nullptr;
    ue::UObject* (*actor)(std::uint8_t kind, std::uint16_t id, std::uint32_t generation) = nullptr;
    void (*motion)(double host_ms) = nullptr;
};
void host_api(HostApi api);
void client_api(ClientApi api);
std::vector<net::proto::Cause> take();
bool root_active(std::uint64_t root);
bool receive(const net::proto::LilyCauses& facts);
bool ready(double host_ms);
void advance(double host_ms, float world_rate = 1);
double playback_ms();
bool target_owned(ue::UObject* actor);
bool allow_stop(ue::UObject* actor, const ability::Identity& id, std::uint8_t reason);
bool event_due(std::uint64_t root, std::uint32_t host_ms);
std::uint64_t current_root();
std::uint16_t source_seat(std::uint64_t root);
std::uint64_t wire_root(std::uint64_t local_root);
void captured(std::uint64_t root, std::uint32_t host_ms, bool sound);
void presented(std::uint64_t root, std::uint32_t host_ms, bool sound);
std::string fault();
void launched(ue::UObject* actor, ue::UObject* invoker, const ability::Identity& id);
void prepare_reuse(ue::UObject* actor);
void stop_requested(ue::UObject* actor, const ability::Identity& id, std::uint8_t reason);
void ended(ue::UObject* actor, const ability::Identity& id);
std::string describe();
std::string trace(std::string_view action);
}
