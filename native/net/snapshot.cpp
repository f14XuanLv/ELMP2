#include "net/snapshot.h"

#include <cmath>
#include <cstring>
#include <type_traits>

namespace elmp::net::snap {

namespace {

enum Tag : std::uint8_t { T_END = 0, T_L = 1, T_E = 2, T_ED = 3, T_W = 4, T_PK = 5, T_SP = 6 };

template <class T>
void put(Writer& w, T v) {
    if constexpr (std::is_same_v<T, float>) w.f32(v);
    else if constexpr (sizeof(T) == 1) w.u8(static_cast<std::uint8_t>(v));
    else if constexpr (sizeof(T) == 2) w.u16(static_cast<std::uint16_t>(v));
    else w.u32(static_cast<std::uint32_t>(v));
}

template <class T>
T get(Reader& r) {
    if constexpr (std::is_same_v<T, float>) {
        const float f = r.f32();
        if (!std::isfinite(f)) r.fail();
        return f;
    } else if constexpr (sizeof(T) == 1) return static_cast<T>(r.u8());
    else if constexpr (sizeof(T) == 2) return static_cast<T>(r.u16());
    else return static_cast<T>(r.u32());
}

template <class Row>
std::uint32_t diff(const Row& a, const Row* b) {
    static_assert(std::tuple_size_v<decltype(Row::fields())> <= 31);
    std::uint32_t mask = 0, bit = 1;
    std::apply([&](auto... f) { ((mask |= (!b || std::memcmp(&(a.*f), &(b->*f), sizeof(a.*f)) != 0) ? bit : 0, bit <<= 1), ...); }, Row::fields());
    return mask;
}

template <class Row>
void write_fields(Writer& w, const Row& r, std::uint32_t mask) {
    std::uint32_t bit = 1;
    std::apply([&](auto... f) { ((mask & bit ? put(w, r.*f) : void(), bit <<= 1), ...); }, Row::fields());
}

template <class Row>
void read_fields(Reader& rd, Row& r, std::uint32_t mask) {
    std::uint32_t bit = 1;
    std::apply([&](auto... f) { ((mask & bit ? void(r.*f = get<std::remove_reference_t<decltype(r.*f)>>(rd)) : void(), bit <<= 1), ...); }, Row::fields());
}

template <class Row, std::size_t Cap>
void enc_section(Writer& w, Tag tag, const Section<Row, Cap>& cur, const Section<Row, Cap>* base) {
    // count changed rows and removed keys first (two passes, no allocation)
    std::uint16_t changed = 0, removed = 0;
    for (std::size_t i = 0; i < cur.n; ++i)
        if (const auto* b = base ? base->find(cur.rows[i].key, i) : nullptr; !b || diff(cur.rows[i], b)) ++changed;
    if (base)
        for (std::size_t i = 0; i < base->n; ++i)
            if (!cur.find(base->rows[i].key, i)) ++removed;
    if (base && !changed && !removed) return; // absent = no change
    w.u8(tag);
    w.var(changed);
    for (std::size_t i = 0; i < cur.n; ++i) {
        const auto& r = cur.rows[i];
        const auto* b = base ? base->find(r.key, i) : nullptr;
        const auto mask = diff(r, b);
        if (b && !mask) continue;
        w.u16(r.key);
        w.var(mask); // varint field mask (rows may carry up to 31 fields)
        write_fields(w, r, mask);
    }
    w.var(removed);
    if (base)
        for (std::size_t i = 0; i < base->n; ++i)
            if (!cur.find(base->rows[i].key, i)) w.u16(base->rows[i].key);
}

template <class Row, std::size_t Cap>
bool dec_section(Reader& rd, Section<Row, Cap>& s) {
    const auto changed = rd.var();
    if (!rd.ok() || changed > Cap) return false;
    constexpr std::uint32_t all = (1u << std::tuple_size_v<decltype(Row::fields())>) - 1;
    for (std::uint32_t i = 0; i < changed; ++i) {
        const auto key = rd.u16();
        const std::uint32_t mask = rd.var();
        if (!rd.ok() || (mask & ~all)) return false;
        Row* r = nullptr;
        for (std::uint16_t k = 0; k < s.n; ++k)
            if (s.rows[k].key == key) r = &s.rows[k];
        if (!r && !(r = s.add(key))) return false;
        read_fields(rd, *r, mask);
    }
    const auto removed = rd.var();
    if (!rd.ok() || removed > Cap) return false;
    for (std::uint32_t i = 0; i < removed; ++i) {
        const auto key = rd.u16();
        for (std::uint16_t k = 0; k < s.n; ++k)
            if (s.rows[k].key == key) {
                s.rows[k] = s.rows[--s.n];
                break;
            }
    }
    return rd.ok();
}

} // namespace

std::size_t encode(const Frame& cur, const Frame* base, std::uint32_t base_seq, std::span<std::uint8_t> out) {
    Writer w(out);
    w.u32(cur.seq);
    w.u32(base ? base_seq : 0);
    w.u32(cur.t_ms);
    w.u16(cur.map);
    w.u16(cur.epoch);
    w.u8(cur.me);
    enc_section(w, T_L, cur.l, base ? &base->l : nullptr);
    enc_section(w, T_E, cur.e, base ? &base->e : nullptr);
    enc_section(w, T_ED, cur.ed, base ? &base->ed : nullptr);
    enc_section(w, T_W, cur.w, base ? &base->w : nullptr);
    enc_section(w, T_PK, cur.pk, base ? &base->pk : nullptr);
    enc_section(w, T_SP, cur.sp, base ? &base->sp : nullptr);
    w.u8(T_END);
    return w.ok() ? w.size() : 0;
}

bool peek(std::span<const std::uint8_t> in, std::uint32_t& seq, std::uint32_t& base_seq) {
    Reader r(in);
    seq = r.u32();
    base_seq = r.u32();
    return r.ok();
}

bool decode(std::span<const std::uint8_t> in, const Frame* base, Frame& out) {
    Reader r(in);
    const auto seq = r.u32();
    const auto base_seq = r.u32();
    if (!r.ok() || (base_seq != 0) != (base != nullptr) || (base && base->seq != base_seq)) return false;
    if (base) {
        if (&out != base) out = *base;
    } else {
        out.clear();
    }
    out.seq = seq;
    out.t_ms = r.u32();
    out.map = r.u16();
    out.epoch = r.u16();
    out.me = r.u8();
    int last = 0;
    while (r.ok()) {
        const auto tag = r.u8();
        if (tag == T_END) break;
        if (tag <= last || tag > T_SP) return false; // each section at most once, in order
        last = tag;
        bool ok = false;
        switch (tag) {
        case T_L: ok = dec_section(r, out.l); break;
        case T_E: ok = dec_section(r, out.e); break;
        case T_ED: ok = dec_section(r, out.ed); break;
        case T_W: ok = dec_section(r, out.w); break;
        case T_PK: ok = dec_section(r, out.pk); break;
        case T_SP: ok = dec_section(r, out.sp); break;
        }
        if (!ok) return false;
    }
    return r.done();
}

} // namespace elmp::net::snap
