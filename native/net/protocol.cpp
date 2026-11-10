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
    w.u32(m.view);
    return done(w);
}
std::size_t encode(const RefreshView& m, std::span<std::uint8_t> out) {
    if (!m.map || !m.view) return 0;
    auto w = start(out, Msg::RefreshView);
    w.u16(m.map); w.u32(m.view);
    return done(w);
}
bool decode(std::span<const std::uint8_t> in, RefreshView& m) {
    Reader r(in);
    if (!open(r, Msg::RefreshView)) return false;
    m.map = r.u16(); m.view = r.u32();
    return r.done() && m.map && m.view;
}
std::size_t encode(const MapReady& m, std::span<std::uint8_t> out) {
    auto w = start(out, Msg::MapReady);
    w.u16(m.map);
    w.u32(m.view);
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
    m.view = r.u32();
    return r.done();
}
bool decode(std::span<const std::uint8_t> in, MapReady& m) {
    Reader r(in);
    if (!open(r, Msg::MapReady)) return false;
    m.map = r.u16();
    m.view = r.u32();
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
    w.u32(m.t_ms); w.u64(m.causal_root);
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
    m.t_ms = r.u32(); m.causal_root = r.u64();
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
    w.u32(m.t_ms);
    return done(w);
}
bool decode(std::span<const std::uint8_t> in, FxStop& m) {
    Reader r(in);
    if (!open(r, Msg::FxStop)) return false;
    m.seq = r.u32();
    m.t_ms = r.u32();
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
    w.u32(m.t_ms); w.u64(m.causal_root);
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
    m.t_ms = r.u32(); m.causal_root = r.u64();
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
std::size_t encode(const LilyFrame& m, std::span<std::uint8_t> out, LilyFrameError* error) {
    const auto reject = [&](std::string_view field, double value, int index = -1) -> std::size_t {
        if (error) *error = {field, value, index};
        return 0;
    };
    if (m.persistent_class) {
        if (!m.persistent_token) return reject("persistent.token", 0);
        if (!m.persistent_input) return reject("persistent.input", 0);
        if (m.persistent_direction > 8) return reject("persistent.direction", m.persistent_direction);
        if (!valid_invoked_input(m.persistent_button)) return reject("persistent.button", m.persistent_button);
    } else if (m.persistent_token || m.persistent_input || m.persistent_direction || m.persistent_button)
        return reject("persistent.class missing", 0);
    if (m.facing < 1 || m.facing > 2) return reject("facing", m.facing);
    if (m.actor_clock_count > kLilyClocks) return reject("actor_clock_count", m.actor_clock_count);
    if (!m.epoch) return reject("epoch", 0);
    if (!m.view) return reject("view", 0);
    if (!std::isfinite(m.world_dt) || m.world_dt <= 0 || m.world_dt > 60) return reject("world_dt", m.world_dt);
    if (!m.seat || m.seat > 8) return reject("seat", m.seat);
    if (!m.map) return reject("map", 0);
    if (!m.seq) return reject("seq", 0);
    if (!m.generation) return reject("generation", 0);
    if (m.direction > 8) return reject("direction", m.direction);
    if (m.mode > 6) return reject("mode", m.mode);
    if (!std::isfinite(m.dt) || m.dt < 0 || m.dt > 60) return reject("dt", m.dt);
    if (!std::isfinite(m.step) || m.step <= 0 || m.step > 60) return reject("step", m.step);
    if (m.state_count > kLilyStates) return reject("state_count", m.state_count);
    if (m.command_state_count > kLilyStates) return reject("command_state_count", m.command_state_count);
    if (m.flags & ~0x3FEu) return reject("flags", m.flags);
    if (m.raw & ~0x3FEu) return reject("raw", m.raw);
    auto w = start(out, Msg::LilyFrame);
    w.u16(m.seat); w.u16(m.map); w.u32(m.seq); w.u32(m.generation); w.f32(m.dt); w.f32(m.step);
    w.u16(m.epoch); w.u32(m.view); w.u32(m.t_ms);
    w.f32(m.world_dt);
    for (int i = 0; i < static_cast<int>(m.input.size()); ++i) {
        if (!std::isfinite(m.input[i])) return reject("input", m.input[i], i);
        w.f32(m.input[i]);
    }
    w.u8(m.direction); w.u8(m.mode); w.u8(m.custom_mode); w.u16(m.flags); w.u16(m.raw);
    for (int i = 0; i < static_cast<int>(m.position.size()); ++i) {
        if (!std::isfinite(m.position[i])) return reject("position", m.position[i], i);
        w.f32(m.position[i]);
    }
    for (int i = 0; i < static_cast<int>(m.velocity.size()); ++i) {
        if (!std::isfinite(m.velocity[i])) return reject("velocity", m.velocity[i], i);
        w.f32(m.velocity[i]);
    }
    w.u8(m.state_count);
    for (std::size_t i = 0; i < m.state_count; ++i) { if (!m.states[i]) return reject("states", 0, static_cast<int>(i)); w.u16(m.states[i]); }
    w.u8(m.command_state_count);
    for (std::size_t i = 0; i < m.command_state_count; ++i) { if (!m.command_states[i]) return reject("command_states", 0, static_cast<int>(i)); w.u16(m.command_states[i]); }
    w.u8(m.facing);w.u8(m.actor_clock_count);
    for (int i = 0; i < m.actor_clock_count; ++i) {
        const auto& c = m.actor_clocks[i];
        if (!c.cls) return reject("clock.class", 0, i);
        if (!std::isfinite(c.scale) || c.scale < 0 || c.scale > 1000) return reject("clock.scale", c.scale, i);
        w.u16(c.cls); w.f32(c.scale);
    }
    if (m.commands.size() > kLilyCommands) return reject("command_count", static_cast<double>(m.commands.size()));
    w.u8(static_cast<std::uint8_t>(m.commands.size()));
    for (int i = 0; i < static_cast<int>(m.commands.size()); ++i) {
        const auto& c = m.commands[i];
        if (!c.token) return reject("command.token", 0, i);
        if (!c.input || c.input > m.seq) return reject("command.input", c.input, i);
        if (!c.cls) return reject("command.class", 0, i);
        if (c.direction > 8) return reject("command.direction", c.direction, i);
        if (!valid_invoked_input(c.button)) return reject("command.button", c.button, i);
        if (c.end > 7) return reject("command.end", c.end, i);
        w.u64(c.token); w.u32(c.input); w.u16(c.cls); w.u8(c.direction); w.u8(c.button); w.u8(c.end);
        w.u8(c.climb_target.has_value());
        if (c.climb_target) {
            if (c.end) return reject("command.climb_target on End", c.end, i);
            for (float coordinate : *c.climb_target) {
                if (!std::isfinite(coordinate)) return reject("command.climb_target", coordinate, i);
                w.f32(coordinate);
            }
        }
    }
    w.u16(m.persistent_class);w.u64(m.persistent_token);w.u32(m.persistent_input);w.u8(m.persistent_direction);w.u8(m.persistent_button);
    return w.ok() ? w.size() : reject("buffer capacity", static_cast<double>(out.size()));
}
bool decode(std::span<const std::uint8_t> in, LilyFrame& m) {
    Reader r(in);
    if (!open(r, Msg::LilyFrame)) return false;
    m.seat = r.u16(); m.map = r.u16(); m.seq = r.u32(); m.generation = r.u32(); m.dt = r.f32(); m.step = r.f32();
    m.epoch = r.u16(); m.view = r.u32(); m.t_ms = r.u32();
    m.world_dt = r.f32();
    for (auto& f : m.input) f = r.f32();
    m.direction = r.u8(); m.mode = r.u8(); m.custom_mode = r.u8(); m.flags = r.u16(); m.raw = r.u16();
    for (auto& f : m.position) f = r.f32();
    for (auto& f : m.velocity) f = r.f32();
    m.state_count = r.u8();
    if (m.state_count > kLilyStates) return false;
    for (std::size_t i = 0; i < m.state_count; ++i) m.states[i] = r.u16();
    m.command_state_count = r.u8();
    if (m.command_state_count > kLilyStates) return false;
    for (std::size_t i = 0; i < m.command_state_count; ++i) m.command_states[i] = r.u16();
    m.facing=r.u8();m.actor_clock_count=r.u8();if(m.actor_clock_count>kLilyClocks)return false;
    for(unsigned i=0;i<m.actor_clock_count;++i){m.actor_clocks[i].cls=r.u16();m.actor_clocks[i].scale=r.f32();}
    const auto count = r.u8();
    if (count > kLilyCommands) return false;
    m.commands.resize(count);
    for (auto& c : m.commands) {
        c.token = r.u64(); c.input = r.u32(); c.cls = r.u16(); c.direction = r.u8(); c.button = r.u8(); c.end = r.u8();
        const auto has_target = r.u8();
        if (has_target > 1 || (has_target && c.end)) return false;
        c.climb_target.reset();
        if (has_target) for (auto& coordinate : c.climb_target.emplace()) coordinate = r.f32();
    }
    m.persistent_class=r.u16();m.persistent_token=r.u64();m.persistent_input=r.u32();m.persistent_direction=r.u8();m.persistent_button=r.u8();
    std::array<std::uint8_t, kLilyFrameBytes> scratch{};
    return r.done() && encode(m, scratch) != 0 && m.mode <= 6 && m.direction <= 9;
}

bool decode(std::span<const std::uint8_t> in, Json& m) {    Reader r(in);
    const auto t = r.u8();
    if (t != static_cast<std::uint8_t>(Msg::Build) && t != static_cast<std::uint8_t>(Msg::EquipRequest) && t != static_cast<std::uint8_t>(Msg::EquipResult) && t != static_cast<std::uint8_t>(Msg::LilyBuild)) return false;
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
