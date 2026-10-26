#pragma once

// Typed world snapshot (§8.1) and its per-peer delta codec. Pure data: no UE types, no strings (names
// travel as u16 ids through the reliable NameDef channel). Fixed-capacity, preallocated rows; encode /
// decode never allocate. Delta = against a snapshot the peer acknowledged (`base`); per section:
// changed / new rows as key + field mask + changed fields, removed rows as an explicit key list.
// A section absent from a packet means "no change" (row residency, v1 net-facts「呈现化架构语义」).

#include "net/codec.h"

#include <array>
#include <cstdint>
#include <span>
#include <tuple>

namespace elmp::net::snap {

struct LilyRow { // key = seat slot (1 = host P1)
    std::uint16_t key = 0;
    float x = 0, y = 0, z = 0;
    std::int8_t facing = 0;
    std::uint8_t alive = 0;
    std::uint16_t hp = 0, mhp = 0, mp = 0, mmp = 0;
    std::uint8_t heals = 0, mheals = 0;
    std::uint16_t anim = 0; // name id
    float t = 0;
    std::uint8_t loop = 0;
    std::uint16_t skin = 0;
    std::uint16_t map = 0; // name id of the map the seat is in
    static constexpr auto fields() {
        return std::make_tuple(&LilyRow::x, &LilyRow::y, &LilyRow::z, &LilyRow::facing, &LilyRow::alive, &LilyRow::hp, &LilyRow::mhp, &LilyRow::mp, &LilyRow::mmp,
                               &LilyRow::heals, &LilyRow::mheals, &LilyRow::anim, &LilyRow::t, &LilyRow::loop, &LilyRow::skin, &LilyRow::map);
    }
};

struct EnemyRow { // key = net id (spawn point name id) or dyn id (section ed)
    std::uint16_t key = 0;
    float x = 0, y = 0, z = 0, vz = 0;
    std::int8_t facing = 0;
    std::uint8_t flags = 0; // bit0 active (not paused), bit1 hidden, bit2 dead (last sample hp <= 0)
    std::uint16_t hp = 0, mhp = 0;
    std::uint16_t type = 0; // class name id
    std::uint16_t anim = 0;
    float t = 0;
    std::uint8_t loop = 0;
    static constexpr auto fields() {
        return std::make_tuple(&EnemyRow::x, &EnemyRow::y, &EnemyRow::z, &EnemyRow::vz, &EnemyRow::facing, &EnemyRow::flags, &EnemyRow::hp, &EnemyRow::mhp,
                               &EnemyRow::type, &EnemyRow::anim, &EnemyRow::t, &EnemyRow::loop);
    }
};

struct WorldRow { // key = object name id; interactables / breakables / story conditions
    std::uint16_t key = 0;
    std::uint8_t flags = 0; // bit0 cleared, bit1 gone (destroyed on the host)
    std::int32_t value = 0; // story: condition result
    static constexpr auto fields() { return std::make_tuple(&WorldRow::flags, &WorldRow::value); }
};

struct PickRow { // key = dyn id of a drop on the host
    std::uint16_t key = 0;
    float x = 0, y = 0, z = 0;
    std::uint16_t cls = 0;
    static constexpr auto fields() { return std::make_tuple(&PickRow::x, &PickRow::y, &PickRow::z, &PickRow::cls); }
};

template <class Row, std::size_t Cap>
struct Section {
    std::array<Row, Cap> rows;
    std::uint16_t n = 0;
    void clear() { n = 0; }
    Row* add(std::uint16_t key) {
        if (n >= Cap) return nullptr;
        rows[n] = Row{};
        rows[n].key = key;
        return &rows[n++];
    }
    const Row* find(std::uint16_t key, std::size_t hint = ~std::size_t{0}) const { // hint: same index first (rows keep order)
        if (hint < n && rows[hint].key == key) return &rows[hint];
        for (std::uint16_t i = 0; i < n; ++i)
            if (rows[i].key == key) return &rows[i];
        return nullptr;
    }
    std::span<const Row> view() const { return {rows.data(), n}; }
};

struct Frame {
    std::uint32_t seq = 0;   // host-assigned, per peer stream
    std::uint32_t t_ms = 0;  // host monotonic ms
    std::uint16_t map = 0;   // host P1 map name id
    std::uint16_t epoch = 0; // world / seat generation
    std::uint8_t me = 0;     // the receiving peer's seat on the host
    Section<LilyRow, 8> l;
    Section<EnemyRow, 128> e;
    Section<EnemyRow, 64> ed;
    Section<WorldRow, 512> w;
    Section<PickRow, 64> pk;
    void clear() {
        l.clear(), e.clear(), ed.clear(), w.clear(), pk.clear();
    }
};

// Encode `cur` relative to `base` (nullptr = full). Returns bytes written, 0 if the buffer was too small.
std::size_t encode(const Frame& cur, const Frame* base, std::uint32_t base_seq, std::span<std::uint8_t> out);
// Peek header: seq and the base it was encoded against.
bool peek(std::span<const std::uint8_t> in, std::uint32_t& seq, std::uint32_t& base_seq);
// Decode into `out` starting from `base` (must be the snapshot with seq == base_seq, or nullptr for full).
// False on any malformed / out-of-range input (out is then unspecified).
bool decode(std::span<const std::uint8_t> in, const Frame* base, Frame& out);

} // namespace elmp::net::snap
