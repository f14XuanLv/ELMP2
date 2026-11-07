#pragma once
#include "engine/ue.h"
#include "net/lily_events.h"
#include <string>
namespace elmp::coop::lily_events {
using Name=std::uint16_t(*)(std::string_view);
using Text=const std::string*(*)(std::uint16_t);
bool capture(ue::UObject* pawn,Name name,net::snap::LilyEvents& out);
bool apply(ue::UObject* pawn,const net::snap::LilyEvents& value,std::uint32_t sample,Text text);
bool capture_layer(ue::UObject*,unsigned,Name,net::snap::EventAnimation&);
bool apply_layer(ue::UObject*,unsigned,const net::snap::EventAnimation&,std::uint32_t,Text);
void reset();
std::string describe();
}
