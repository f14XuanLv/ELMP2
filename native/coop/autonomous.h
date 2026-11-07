#pragma once
#include "coop/native_lily.h"
#include "coop/causality.h"
namespace elmp::coop::autonomous {
using FrameSink = void(*)(net::proto::SpiritFrame&);
bool host_tick(FrameSink, native_lily::Name);
bool client_tick(std::uint16_t seat, std::uint16_t map, native_lily::Text);
void receive(const net::proto::SpiritFrame&);
void summoned(ue::UObject* spirit, ue::UObject* command);
void command_started(ue::UObject* command, ue::UObject* pawn);
void command_ended(ue::UObject* command, ue::UObject* pawn, std::uint8_t reason);
bool source(ue::UObject*, native_lily::Source&);
bool owns(ue::UObject*);
ue::UObject* summoner(ue::UObject*);
bool eligible(ue::UObject*);
void reset();
std::string fault();
std::string describe();
}
