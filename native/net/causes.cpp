#include "net/protocol.h"
#include <cmath>

namespace elmp::net::proto {
std::size_t encode(const SpiritFrame& m,std::span<std::uint8_t> out) {
    if (!m.context.commands.empty()) return 0;
    if(!m.role||!m.episode||!m.cls||!m.summon||m.edges.size()>16||m.layer>9||!std::isfinite(m.actor_scale)||m.actor_scale<0||m.actor_scale>1000)return 0;
    std::array<std::uint8_t,512> data{};const auto size=encode(m.context,data);if(!size)return 0;
    Writer w(out);w.u8(static_cast<std::uint8_t>(Msg::SpiritFrame));w.u16(static_cast<std::uint16_t>(size));w.bytes(std::span(data.data(),size));
    w.u32(m.role);w.u32(m.episode);w.u32(m.summon_input);w.u32(m.command_sequence);w.u16(m.cls);w.u16(m.summon);w.f32(m.actor_scale);w.u8((m.active?1:0)|(m.decision_only?2:0)|(m.soft_ending?4:0));w.u8(static_cast<std::uint8_t>(m.edges.size()));
    for(const auto& e:m.edges){if(!e.sequence||!e.cls||e.kind>1||e.direction>8||e.button>9||e.reason>6)return 0;w.u32(e.sequence);w.u16(e.cls);w.u8(e.kind);w.u8(e.direction);w.u8(e.button);w.u8(e.reason);}
    const auto& a=m.animation;
    if((a.generation==0)!=(a.animation==0)||a.flags>3)return 0;
    w.u8(m.layer);w.u32(a.generation);w.u16(a.animation);w.u8(a.flags);
    for(float f:{a.time,a.start,a.end,a.blend}){if(!std::isfinite(f)||f<0)return 0;w.f32(f);}
    w.u8(m.has_follow?1:0);for(auto* v:{&m.follow_position,&m.follow_rotation,&m.follow_velocity,&m.follow_scale})for(float f:*v){if(!std::isfinite(f))return 0;w.f32(f);}
    for(float f:m.follow_history){if(!std::isfinite(f))return 0;w.f32(f);}
    for(float f:m.movement_input){if(!std::isfinite(f)||std::abs(f)>1e6f)return 0;w.f32(f);}
    if(m.actor_direction>8||m.forced_walk>3||(m.actor_flags&~0x3FEu)||(m.actor_raw&~0x3FEu))return 0;
    for(float f:m.actor_input){if(!std::isfinite(f)||f<-1.01f||f>1.01f)return 0;w.f32(f);}
    w.u16(m.actor_flags);w.u16(m.actor_raw);w.u8(m.actor_direction);w.u8(m.forced_walk);
    return w.ok()?w.size():0;
}
bool decode(std::span<const std::uint8_t> in,SpiritFrame& m) {
    m={};Reader r(in);if(r.u8()!=static_cast<std::uint8_t>(Msg::SpiritFrame))return false;
    const auto size=r.u16();std::array<std::uint8_t,512> data{};if(size>data.size())return false;r.raw(data.data(),size);if(!r.ok()||!decode(std::span(data.data(),size),m.context))return false;
    m.role=r.u32();m.episode=r.u32();m.summon_input=r.u32();m.command_sequence=r.u32();m.cls=r.u16();m.summon=r.u16();m.actor_scale=r.f32();auto active=r.u8();if(active>7)return false;m.active=(active&1)!=0;m.decision_only=(active&2)!=0;m.soft_ending=(active&4)!=0;
    const auto count=r.u8();if(count>16)return false;m.edges.resize(count);for(auto& e:m.edges){e.sequence=r.u32();e.cls=r.u16();e.kind=r.u8();e.direction=r.u8();e.button=r.u8();e.reason=r.u8();}
    auto& a=m.animation;m.layer=r.u8();a.generation=r.u32();a.animation=r.u16();a.flags=r.u8();a.time=r.f32();a.start=r.f32();a.end=r.f32();a.blend=r.f32();
    const auto follow=r.u8();if(follow>1)return false;m.has_follow=follow!=0;for(auto* v:{&m.follow_position,&m.follow_rotation,&m.follow_velocity,&m.follow_scale})for(float& f:*v)f=r.f32();
    for(float& f:m.follow_history)f=r.f32();
    for(float& f:m.movement_input)f=r.f32();for(float& f:m.actor_input)f=r.f32();m.actor_flags=r.u16();m.actor_raw=r.u16();m.actor_direction=r.u8();m.forced_walk=r.u8();
    std::array<std::uint8_t,1024> scratch{};return r.done()&&encode(m,scratch)!=0;
}
namespace {
bool valid(const Cause& f, std::uint16_t map) {
    if ((f.domain == 1) != (f.role != 0) || f.domain > 1 || static_cast<unsigned>(f.kind) > 3 || !f.seat || f.seat > 8 || f.map != map || !f.root || !f.generation || !f.input || !f.cls || f.path.depth > kAbilityPathDepth || f.flags > 3 || f.reason > 3 || f.ref_kind > 2) return false;
    if ((f.ref_kind == 0) != (f.ref_id == 0) || (f.ref_kind == 0) != (f.ref_generation == 0) || (!f.ref_kind && f.component)) return false;
    if (!std::isfinite(f.min) || !std::isfinite(f.range) || f.min < 0 || f.range < 0) return false;
    if (!std::isfinite(f.actor_scale) || f.actor_scale < 0 || f.actor_scale > 1000 || f.channel > 31) return false;
    for (std::size_t i = 0; i < f.path.depth; ++i) if (!f.path.occurrences[i]) return false;
    for (auto* a : {&f.actor_position, &f.target_position, &f.target_velocity,&f.actor_rotation}) for (float n : *a) if (!std::isfinite(n)) return false;
    if (f.kind == CauseKind::Root && (f.path.depth || !f.ordinal || !f.invoker)) return false;
    if (f.kind == CauseKind::Query && !f.query) return false;
    if (f.kind == CauseKind::Target && f.ref_kind && !f.component) return false;
    return true;
}
void put(Writer& w, const Cause& f) {
    w.u8(static_cast<std::uint8_t>(f.kind));w.u8(f.domain);w.u32(f.role);w.u16(f.seat);w.u32(f.generation);w.u32(f.input);w.u32(f.t_ms);w.u64(f.root);
    w.u16(f.ordinal);w.u16(f.cls);w.u16(f.invoker);w.u16(f.query);w.u8(f.path.depth);
    for (std::size_t i=0;i<f.path.depth;++i){w.u16(f.path.entries[i]);w.u32(f.path.occurrences[i]);}
    w.u8(f.ref_kind);w.u16(f.ref_id);w.u32(f.ref_generation);w.u16(f.component);w.u8(f.flags);w.u8(f.channel);w.u8(f.reason);
    w.f32(f.min);w.f32(f.range);w.f32(f.actor_scale);for(auto* a:{&f.actor_position,&f.target_position,&f.target_velocity,&f.actor_rotation})for(float n:*a)w.f32(n);
}
bool get(Reader& r, Cause& f) {
    f.kind=static_cast<CauseKind>(r.u8());f.domain=r.u8();f.role=r.u32();f.seat=r.u16();f.generation=r.u32();f.input=r.u32();f.t_ms=r.u32();f.root=r.u64();
    f.ordinal=r.u16();f.cls=r.u16();f.invoker=r.u16();f.query=r.u16();f.path.depth=r.u8();
    if(f.path.depth>kAbilityPathDepth)return false;
    for(std::size_t i=0;i<f.path.depth;++i){f.path.entries[i]=r.u16();f.path.occurrences[i]=r.u32();}
    f.ref_kind=r.u8();f.ref_id=r.u16();f.ref_generation=r.u32();f.component=r.u16();f.flags=r.u8();f.channel=r.u8();f.reason=r.u8();
    f.min=r.f32();f.range=r.f32();f.actor_scale=r.f32();for(auto* a:{&f.actor_position,&f.target_position,&f.target_velocity,&f.actor_rotation})for(float& n:*a)n=r.f32();
    return r.ok();
}
}
std::size_t encode(const LilyCauses& m, std::span<std::uint8_t> out) {
    if(!m.epoch||!m.map||!m.view||m.facts.size()>kCauses)return 0;
    Writer w(out);w.u8(static_cast<std::uint8_t>(Msg::LilyCauses));w.u16(m.epoch);w.u16(m.map);w.u32(m.view);w.u32(m.fence_ms);w.u16(static_cast<std::uint16_t>(m.facts.size()));
    for(const auto& f:m.facts){if(!valid(f,m.map))return 0;put(w,f);}return w.ok()?w.size():0;
}
bool decode(std::span<const std::uint8_t> in,LilyCauses& m) {
    Reader r(in);if(r.u8()!=static_cast<std::uint8_t>(Msg::LilyCauses))return false;
    m.epoch=r.u16();m.map=r.u16();m.view=r.u32();m.fence_ms=r.u32();const auto n=r.u16();
    if(!r.ok()||!m.epoch||!m.map||!m.view||n>kCauses)return false;
    m.facts.clear();m.facts.resize(n);for(auto& f:m.facts){f.map=m.map;if(!get(r,f)||!valid(f,m.map))return false;}return r.done();
}
}
