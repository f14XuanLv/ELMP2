#pragma once

#include "engine/ue.h"
#include "net/protocol.h"
#include <string>

namespace elmp::coop::native_lily {
using FrameSink = void (*)(ue::UObject*, net::proto::LilyFrame&);
using Name = std::uint16_t (*)(std::string_view);
using Text = const std::string* (*)(std::uint16_t);
bool host_enable(FrameSink sink, Name name);
void host_disable();
std::string bind(std::uint16_t seat, std::int64_t slot, ue::UObject* pawn);
void build(std::uint16_t seat, const std::string& json);
void receive(const net::proto::LilyFrame& value);
void client_tick(std::uint16_t map, Text text);
void forget(std::uint16_t seat);
void reset();
ue::UObject* actor(std::uint16_t seat);
bool owns(ue::UObject* object); // native avatar or one of its original owned/possessed descendants
struct Source { std::uint16_t seat = 0; std::uint32_t generation = 0, seq = 0; bool command_tick = false; std::uint8_t domain = 0; };
bool source(ue::UObject* object, Source& out);
std::string fault();
std::string describe();
}
