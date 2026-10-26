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
    w.u8(m.dyn);
    w.u32(m.seq);
    w.u16(m.ability);
    for (float f : m.xf) w.f32(f);
    w.u16(m.spawn_bone);
    w.u16(m.attach_bone);
    w.u8(m.flags);
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
    m.dyn = r.u8();
    m.seq = r.u32();
    m.ability = r.u16();
    for (auto& f : m.xf) {
        f = r.f32();
        if (!std::isfinite(f) || std::fabs(f) > 1e6f) r.fail();
    }
    m.spawn_bone = r.u16();
    m.attach_bone = r.u16();
    m.flags = r.u8();
    const float q = m.xf[0] * m.xf[0] + m.xf[1] * m.xf[1] + m.xf[2] * m.xf[2] + m.xf[3] * m.xf[3];
    return r.done() && m.ability != 0 && m.dyn <= 1 && q > 0.5f && q < 1.5f && !(m.flags & ~7u);
}
bool decode(std::span<const std::uint8_t> in, Json& m) {
    Reader r(in);
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
