#include "net/protocol.h"
#include <cmath>

namespace elmp::net::proto {
namespace {
bool valid(const Cause& f, std::uint16_t map) {
    if (static_cast<unsigned>(f.kind) > 3 || !f.seat || f.seat > 8 || f.map != map || !f.root || !f.generation || !f.input || !f.cls || f.path.depth > kAbilityPathDepth || f.flags > 3 || f.reason > 3 || f.ref_kind > 2) return false;
    if ((f.ref_kind == 0) != (f.ref_id == 0) || (f.ref_kind == 0) != (f.ref_generation == 0) || (!f.ref_kind && f.component)) return false;
    if (!std::isfinite(f.min) || !std::isfinite(f.range) || f.min < 0 || f.range < 0) return false;
    if (!std::isfinite(f.actor_scale) || f.actor_scale < 0 || f.actor_scale > 1000 || f.channel > 31) return false;
    for (std::size_t i = 0; i < f.path.depth; ++i) if (!f.path.occurrences[i]) return false;
    for (auto* a : {&f.actor_position, &f.target_position, &f.target_velocity}) for (float n : *a) if (!std::isfinite(n)) return false;
    if (f.kind == CauseKind::Root && (f.path.depth || !f.ordinal || !f.invoker)) return false;
    if (f.kind == CauseKind::Query && !f.query) return false;
    if (f.kind == CauseKind::Target && f.ref_kind && !f.component) return false;
    return true;
}
void put(Writer& w, const Cause& f) {
    w.u8(static_cast<std::uint8_t>(f.kind));w.u16(f.seat);w.u32(f.generation);w.u32(f.input);w.u32(f.t_ms);w.u64(f.root);
    w.u16(f.ordinal);w.u16(f.cls);w.u16(f.invoker);w.u16(f.query);w.u8(f.path.depth);
    for (std::size_t i=0;i<f.path.depth;++i){w.u16(f.path.entries[i]);w.u32(f.path.occurrences[i]);}
    w.u8(f.ref_kind);w.u16(f.ref_id);w.u32(f.ref_generation);w.u16(f.component);w.u8(f.flags);w.u8(f.channel);w.u8(f.reason);
    w.f32(f.min);w.f32(f.range);w.f32(f.actor_scale);for(auto* a:{&f.actor_position,&f.target_position,&f.target_velocity})for(float n:*a)w.f32(n);
}
bool get(Reader& r, Cause& f) {
    f.kind=static_cast<CauseKind>(r.u8());f.seat=r.u16();f.generation=r.u32();f.input=r.u32();f.t_ms=r.u32();f.root=r.u64();
    f.ordinal=r.u16();f.cls=r.u16();f.invoker=r.u16();f.query=r.u16();f.path.depth=r.u8();
    if(f.path.depth>kAbilityPathDepth)return false;
    for(std::size_t i=0;i<f.path.depth;++i){f.path.entries[i]=r.u16();f.path.occurrences[i]=r.u32();}
    f.ref_kind=r.u8();f.ref_id=r.u16();f.ref_generation=r.u32();f.component=r.u16();f.flags=r.u8();f.channel=r.u8();f.reason=r.u8();
    f.min=r.f32();f.range=r.f32();f.actor_scale=r.f32();for(auto* a:{&f.actor_position,&f.target_position,&f.target_velocity})for(float& n:*a)n=r.f32();
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
