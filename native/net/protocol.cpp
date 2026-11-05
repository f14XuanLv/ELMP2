#include "net/protocol.h"

#include <cmath>

namespace elmp::net::proto {

namespace {

std::size_t done(const Writer& w) { return w.ok() ? w.size() : 0; }
Writer start(std::span<std::uint8_t> out, Msg t) {
    Writer w(out);
    w.u8(static_cast<std::uint8_t>(t));
    return w;
}
bool open(Reader& r, Msg t) { return r.u8() == static_cast<std::uint8_t>(t) && r.ok(); }
bool kind_ok(std::uint8_t k) { return k >= 1 && k <= kRefKindMax; }
void read_xf(Reader& r, std::array<float, 10>& xf) {
    for (auto& f : xf) {
        f = r.f32();
        if (!std::isfinite(f) || std::fabs(f) > 1e6f) r.fail();
    }
}
bool unit(const std::array<float, 10>& x) {
    const float q = x[0] * x[0] + x[1] * x[1] + x[2] * x[2] + x[3] * x[3];
    return q > 0.5f && q < 1.5f;
}
bool sound_ok(const Snd& m) {
    if (!m.event || !kind_ok(m.kind) || m.is2d > 1 || (m.flags & ~3u) || m.flags == 1) return false;
    if (!(m.flags & 2)) return true;
    for (const auto f : m.xf) if (!std::isfinite(f) || std::fabs(f) > 1e6f) return false;
    return unit(m.xf);
}

} // namespace

std::size_t encode(const Input& in, std::span<std::uint8_t> out) {
    Writer w(out);
    w.u16(in.epoch);
    w.i8(in.x);
    w.i8(in.y);
    w.u16(in.raw);
    for (int i = 1; i < 10; ++i) w.u16(in.presses[i]);
    w.u32(in.snap_ack);
    return done(w);
}

bool decode(std::span<const std::uint8_t> in, Input& out) {
    Reader r(in);
    out.epoch = r.u16();
    out.x = r.i8();
    out.y = r.i8();
    out.raw = r.u16();
    for (int i = 1; i < 10; ++i) out.presses[i] = r.u16();
    out.snap_ack = r.u32();
    return r.done() && out.x >= -100 && out.x <= 100 && out.y >= -100 && out.y <= 100 && !(out.raw & ~0x3FEu);
}

std::size_t encode(const Welcome& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::Welcome);
    w.u8(m.seat);
    w.u16(m.epoch);
    w.u16(m.map);
    return done(w);
}
std::size_t encode(const NameDef& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::NameDef);
    w.u16(m.id);
    w.str(m.text.substr(0, kMaxName));
    return done(w);
}
std::size_t encode(const MapMsg& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::Map);
    w.u16(m.map);
    w.str(m.tag.substr(0, kMaxName));
    return done(w);
}
std::size_t encode(const MapReady& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::MapReady);
    w.u16(m.map);
    return done(w);
}
std::size_t encode(const Act& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::Act);
    w.u16(m.net_id);
    w.u8(m.kind);
    w.u32(m.seq);
    w.u16(m.ability);
    for (float f : m.xf) w.f32(f);
    for (float f : m.xf2) w.f32(f);
    w.u8(m.spawn_bone);
    w.u8(m.attach_bone);
    w.u16(m.custom_spawn);
    w.u16(m.custom_attach);
    w.u8(m.flags);
    w.f32(m.max_execution_time);
    w.f32(m.damage_rate);
    return done(w);
}
std::size_t encode(Msg type, const Json& m, std::span<std::uint8_t> out) {
    auto w = start(out, type);
    if (m.text.size() > kMaxJson) return 0;
    w.str(m.text);
    return done(w);
}
std::size_t encode(const Control& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::Control);
    w.u8(m.code);
    w.str(m.text.substr(0, kMaxName));
    return done(w);
}

Msg type_of(std::span<const std::uint8_t> in) { return in.empty() ? Msg{} : static_cast<Msg>(in[0]); }

bool valid_reaction(const Reaction& m) noexcept {
    if (m.kind > 2 || !m.seat || m.seat > 8 || !m.epoch || !m.map || !m.seq || m.state_count > kReactionStates) return false;
    if (m.kind == 2) return m.state_count == 0;
    for (std::size_t i = 0; i < m.state_count; ++i) if (!m.states[i]) return false;
    if (m.kind == 1) {
        const float effective = m.duration * m.factor;
        return std::isfinite(m.duration) && m.duration > 0 && m.duration <= 1e6f &&
               std::isfinite(m.factor) && m.factor > 0 && m.factor <= 1e6f &&
               std::isfinite(effective) && effective > 0 && effective <= 1e6f;
    }
    if (m.flags > 15 || m.movement_mode > 6 || m.knockback_type > 1 || m.context > 3 || ((m.context & 2) && !(m.context & 1))) return false;
    for (const auto* v : {&m.ground, &m.air, &m.swim, &m.direction, &m.position, &m.velocity, &m.launch_factor, &m.effective_launch})
        for (float f : *v) if (!std::isfinite(f) || std::fabs(f) > 1e7f) return false;
    for (float f : {m.stun, m.move, m.strength, m.movement_factor}) if (!std::isfinite(f) || std::fabs(f) > 1e6f) return false;
    return true;
}

std::size_t encode(const Reaction& m, std::span<std::uint8_t> out) {
    if (!valid_reaction(m)) return 0;
    auto w = start(out, Msg::Reaction);
    w.u8(m.kind); w.u8(m.seat); w.u16(m.epoch); w.u16(m.map); w.u32(m.seq); w.u32(m.t_ms);
    w.u8(m.state_count);
    for (std::size_t i = 0; i < m.state_count; ++i) w.u16(m.states[i]);
    if (m.kind == 2) return done(w);
    if (m.kind == 1) { w.f32(m.duration); w.f32(m.factor); return done(w); }
    w.u8(m.flags);
    for (const auto* v : {&m.ground, &m.air, &m.swim}) for (float f : *v) w.f32(f);
    w.f32(m.stun); w.f32(m.move); w.f32(m.strength); w.u16(m.curve);
    for (const auto* v : {&m.direction, &m.position, &m.velocity, &m.launch_factor, &m.effective_launch}) for (float f : *v) w.f32(f);
    w.f32(m.movement_factor); w.u8(m.movement_mode); w.u8(m.custom_mode); w.u8(m.context); w.u8(m.knockback_type);
    return done(w);
}

bool decode(std::span<const std::uint8_t> in, Reaction& m) {
    Reader r(in);
    if (!open(r, Msg::Reaction)) return false;
    m = {};
    m.kind = r.u8(); m.seat = r.u8(); m.epoch = r.u16(); m.map = r.u16(); m.seq = r.u32(); m.t_ms = r.u32();
    m.state_count = r.u8();
    if (m.kind > 2 || m.state_count > kReactionStates || (m.kind == 2 && m.state_count)) return false;
    for (std::size_t i = 0; i < m.state_count; ++i) m.states[i] = r.u16();
    if (m.kind == 2) return r.done() && valid_reaction(m);
    if (m.kind == 1) { m.duration = r.f32(); m.factor = r.f32(); return r.done() && valid_reaction(m); }
    m.flags = r.u8();
    for (auto* v : {&m.ground, &m.air, &m.swim}) for (float& f : *v) f = r.f32();
    m.stun = r.f32(); m.move = r.f32(); m.strength = r.f32(); m.curve = r.u16();
    for (auto* v : {&m.direction, &m.position, &m.velocity, &m.launch_factor, &m.effective_launch}) for (float& f : *v) f = r.f32();
    m.movement_factor = r.f32(); m.movement_mode = r.u8(); m.custom_mode = r.u8(); m.context = r.u8(); m.knockback_type = r.u8();
    return r.done() && valid_reaction(m);
}

bool decode(std::span<const std::uint8_t> in, Welcome& m) {
    Reader r(in);
    if (!open(r, Msg::Welcome)) return false;
    m.seat = r.u8();
    m.epoch = r.u16();
    m.map = r.u16();
    return r.done();
}
bool decode(std::span<const std::uint8_t> in, NameDef& m) {
    Reader r(in);
    if (!open(r, Msg::NameDef)) return false;
    m.id = r.u16();
    m.text = std::string(r.str(kMaxName));
    return r.done() && m.id != 0;
}
bool decode(std::span<const std::uint8_t> in, MapMsg& m) {
    Reader r(in);
    if (!open(r, Msg::Map)) return false;
    m.map = r.u16();
    m.tag = std::string(r.str(kMaxName));
    return r.done();
}
bool decode(std::span<const std::uint8_t> in, MapReady& m) {
    Reader r(in);
    if (!open(r, Msg::MapReady)) return false;
    m.map = r.u16();
    return r.done();
}
bool decode(std::span<const std::uint8_t> in, Act& m) {
    Reader r(in);
    if (!open(r, Msg::Act)) return false;
    m.net_id = r.u16();
    m.kind = r.u8();
    m.seq = r.u32();
    m.ability = r.u16();
    for (auto* xf : {&m.xf, &m.xf2}) read_xf(r, *xf);
    m.spawn_bone = r.u8();
    m.attach_bone = r.u8();
    m.custom_spawn = r.u16();
    m.custom_attach = r.u16();
    m.flags = r.u8();
    m.max_execution_time = r.f32();
    m.damage_rate = r.f32();
    return r.done() && m.ability != 0 && kind_ok(m.kind) && unit(m.xf) && unit(m.xf2) && !(m.flags & ~7u) &&
           std::isfinite(m.max_execution_time) && std::isfinite(m.damage_rate);
}
std::size_t encode(const Fx& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::Fx);
    w.u8(m.kind);
    w.u16(m.net_id);
    w.u32(m.seq);
    w.u16(m.ps);
    w.u8(m.bone);
    w.u16(m.custom_bone);
    for (float f : m.xf) w.f32(f);
    w.u8(m.flags);
    w.u32(static_cast<std::uint32_t>(m.sort));
    w.u8(m.pool);
    w.u8(m.attach_kind);
    w.u16(m.attach_component);
    w.u16(m.attach_socket);
    w.u8(m.emitter_flags);
    return done(w);
}
bool decode(std::span<const std::uint8_t> in, Fx& m) {
    Reader r(in);
    if (!open(r, Msg::Fx)) return false;
    m.kind = r.u8();
    m.net_id = r.u16();
    m.seq = r.u32();
    m.ps = r.u16();
    m.bone = r.u8();
    m.custom_bone = r.u16();
    read_xf(r, m.xf);
    m.flags = r.u8();
    m.sort = static_cast<std::int32_t>(r.u32());
    m.pool = r.u8();
    m.attach_kind = r.u8();
    m.attach_component = r.u16();
    m.attach_socket = r.u16();
    m.emitter_flags = r.u8();
    if (!r.done() || !m.ps || !kind_ok(m.kind) || (m.flags & ~63u) || m.pool > 3) return false;
    if (!(m.flags & 8)) return unit(m.xf) && !m.attach_kind && !m.attach_component && !m.attach_socket && !m.emitter_flags;
    const bool path = m.attach_kind == static_cast<std::uint8_t>(FxAttach::Component) || m.attach_kind == static_cast<std::uint8_t>(FxAttach::SpineBone);
    const bool bone = m.attach_kind == static_cast<std::uint8_t>(FxAttach::SpineBone);
    return (m.flags & 1) && m.attach_kind >= 1 && m.attach_kind <= 4 && path == (m.attach_component != 0) &&
           (bone ? valid_attach_bone(m.bone) : (!m.bone && !(m.flags & 2))) &&
           !m.custom_bone && !(m.emitter_flags & ~3u) && m.sort >= 0 && m.sort <= 3 && m.xf[3] == 1.0f;
}
std::size_t encode(const FxStop& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::FxStop);
    w.u32(m.seq);
    return done(w);
}
bool decode(std::span<const std::uint8_t> in, FxStop& m) {
    Reader r(in);
    if (!open(r, Msg::FxStop)) return false;
    m.seq = r.u32();
    return r.done();
}
std::size_t encode(const Snd& m, std::span<std::uint8_t> out) {
    if (!sound_ok(m)) return 0;
    auto w = start(out, Msg::Snd);
    w.u8(m.kind);
    w.u16(m.net_id);
    w.u16(m.event);
    w.u8(m.is2d);
    w.u8(m.flags);
    if (m.flags & 2) for (const auto f : m.xf) w.f32(f);
    return done(w);
}
bool decode(std::span<const std::uint8_t> in, Snd& m) {
    Reader r(in);
    if (!open(r, Msg::Snd)) return false;
    m = {}; // optional transform must not retain a previous packet's exact coordinates
    m.kind = r.u8();
    m.net_id = r.u16();
    m.event = r.u16();
    m.is2d = r.u8();
    m.flags = r.u8();
    if (m.flags & 2) read_xf(r, m.xf);
    return r.done() && sound_ok(m);
}
std::size_t encode(const ActStop& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::ActStop);
    w.u8(m.kind);
    w.u16(m.net_id);
    w.u32(m.seq);
    w.u8(m.reason);
    return done(w);
}
bool decode(std::span<const std::uint8_t> in, ActStop& m) {
    Reader r(in);
    if (!open(r, Msg::ActStop)) return false;
    m.kind = r.u8();
    m.net_id = r.u16();
    m.seq = r.u32();
    m.reason = r.u8();
    return r.done() && kind_ok(m.kind) && m.reason < 16;
}
bool valid_child_stop(const ChildStop& m) noexcept {
    if (!kind_ok(m.kind) || !m.net_id || !m.seq || !m.ability || !m.path.depth || m.path.depth > kAbilityPathDepth || m.reason > 3) return false;
    for (std::size_t i = 0; i < m.path.depth; ++i) if (!m.path.occurrences[i]) return false;
    return true;
}
std::size_t encode(const ChildStop& m, std::span<std::uint8_t> out) {
    if (!valid_child_stop(m)) return 0;
    auto w = start(out, Msg::ChildStop);
    w.u8(m.kind); w.u16(m.net_id); w.u32(m.seq); w.u16(m.ability); w.u8(m.path.depth);
    for (std::size_t i = 0; i < m.path.depth; ++i) { w.u16(m.path.entries[i]); w.u32(m.path.occurrences[i]); }
    w.u8(m.reason);
    return done(w);
}
bool decode(std::span<const std::uint8_t> in, ChildStop& m) {
    Reader r(in);
    if (!open(r, Msg::ChildStop)) return false;
    m = {};
    m.kind = r.u8(); m.net_id = r.u16(); m.seq = r.u32(); m.ability = r.u16(); m.path.depth = r.u8();
    if (!m.path.depth || m.path.depth > kAbilityPathDepth) return false;
    for (std::size_t i = 0; i < m.path.depth; ++i) { m.path.entries[i] = r.u16(); m.path.occurrences[i] = r.u32(); }
    m.reason = r.u8();
    return r.done() && valid_child_stop(m);
}
bool decode(std::span<const std::uint8_t> in, Json& m) {    Reader r(in);
    const auto t = r.u8();
    if (t != static_cast<std::uint8_t>(Msg::Build) && t != static_cast<std::uint8_t>(Msg::EquipRequest) && t != static_cast<std::uint8_t>(Msg::EquipResult)) return false;
    m.text = std::string(r.str(kMaxJson));
    return r.done();
}
bool decode(std::span<const std::uint8_t> in, Control& m) {
    Reader r(in);
    if (!open(r, Msg::Control)) return false;
    m.code = r.u8();
    m.text = std::string(r.str(kMaxName));
    return r.done();
}

std::uint16_t NameTable::intern(std::string_view s, bool* is_new) {
    if (is_new) *is_new = false;
    if (auto it = ids_.find(std::string(s)); it != ids_.end()) return it->second;
    if (names_.size() >= 65535 || s.size() > kMaxName) return 0;
    names_.emplace_back(s);
    const auto id = static_cast<std::uint16_t>(names_.size());
    ids_.emplace(names_.back(), id);
    if (is_new) *is_new = true;
    return id;
}
const std::string* NameTable::text(std::uint16_t id) const { return id && id <= names_.size() && !names_[id - 1].empty() ? &names_[id - 1] : nullptr; }
void NameTable::define(std::uint16_t id, std::string_view s) {
    if (!id) return;
    if (names_.size() < id) names_.resize(id);
    names_[id - 1] = s;
    ids_[std::string(s)] = id;
}
void NameTable::clear() {
    names_.clear();
    ids_.clear();
}

} // namespace elmp::net::proto
