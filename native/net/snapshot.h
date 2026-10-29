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
#include <type_traits>

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
    // 呈现用的骨骼下移量（0.1 UU）：原版 USpineAnimationComponent 在 AfterBoneTransformUpdate 里按骨架把
    // SpineSkeletonRenderer 往下挪（莉莉 -65、e2021 -50），这样精灵的脚才落在地面上。客户端不跑那条 tick，
    // 于是自建 Pawn 的渲染器停在 0 → 整个人"高一个身位"。把 Host 的实测值随行发过来即可，不写死数值。
    std::int16_t zoff = 0;
    static constexpr auto fields() {
        return std::make_tuple(&LilyRow::x, &LilyRow::y, &LilyRow::z, &LilyRow::facing, &LilyRow::alive, &LilyRow::hp, &LilyRow::mhp, &LilyRow::mp, &LilyRow::mmp,
                               &LilyRow::heals, &LilyRow::mheals, &LilyRow::anim, &LilyRow::t, &LilyRow::loop, &LilyRow::skin, &LilyRow::map, &LilyRow::zoff);
    }
};

struct EnemyRow { // key = net id (spawn point name id) or dyn id (section ed)
    std::uint16_t key = 0;
    float x = 0, y = 0, z = 0, vz = 0;
    std::int8_t facing = 0;
    std::uint8_t flags = 0; // bit0 active (not paused), bit1 hidden, bit2 dead (last sample hp <= 0),
                            // bit3 engaged (host pawn got a target: BattleColor red tint + TriggerFindEnemyFX)
    std::uint16_t hp = 0, mhp = 0;
    std::uint16_t type = 0; // class name id
    std::uint16_t anim = 0;
    float t = 0;
    std::uint8_t loop = 0;
    std::int16_t zoff = 0; // 渲染组件的相对 Z（0.1 UU）：见 LilyRow 上的说明，敌人是 -50 这类"踩地"偏移
    static constexpr auto fields() {
        return std::make_tuple(&EnemyRow::x, &EnemyRow::y, &EnemyRow::z, &EnemyRow::vz, &EnemyRow::facing, &EnemyRow::flags, &EnemyRow::hp, &EnemyRow::mhp,
                               &EnemyRow::type, &EnemyRow::anim, &EnemyRow::t, &EnemyRow::loop, &EnemyRow::zoff);
    }
};

struct WorldRow { // key = object name id; interactables / breakables / story conditions
    std::uint16_t key = 0;
    std::uint8_t flags = 0; // bit0 cleared, bit1 gone (destroyed on the host), bit2 broken (breakable HP ≤ 0)
    std::int32_t value = 0; // story: condition result
    static constexpr auto fields() { return std::make_tuple(&WorldRow::flags, &WorldRow::value); }
};

struct PickRow { // key = dyn id of a drop on the host
    std::uint16_t key = 0;
    float x = 0, y = 0, z = 0;
    std::uint16_t cls = 0;
    static constexpr auto fields() { return std::make_tuple(&PickRow::x, &PickRow::y, &PickRow::z, &PickRow::cls); }
};

struct SpiritRow { // key = seat << 8 | per-seat index; only visible (summoned) spirits have rows
    std::uint16_t key = 0;
    std::uint16_t cls = 0; // class path name id
    float x = 0, y = 0, z = 0;
    std::int8_t facing = 0;
    std::uint16_t anim = 0;
    float t = 0;
    std::uint8_t loop = 0;
    static constexpr auto fields() {
        return std::make_tuple(&SpiritRow::cls, &SpiritRow::x, &SpiritRow::y, &SpiritRow::z, &SpiritRow::facing, &SpiritRow::anim, &SpiritRow::t, &SpiritRow::loop);
    }
};

// The delta codec carries one mask bit per field (snapshot.cpp). It used to be 16 bits: LilyRow grew past 16
// and the extra field was silently never transmitted (zoff, 2026-10-30) — the mask is 32 bits now, and these
// asserts keep a future field from falling off the end again.
static_assert(std::tuple_size_v<decltype(LilyRow::fields())> <= 32, "LilyRow exceeds the 32-field mask");
static_assert(std::tuple_size_v<decltype(EnemyRow::fields())> <= 32, "EnemyRow exceeds the 32-field mask");
static_assert(std::tuple_size_v<decltype(WorldRow::fields())> <= 32, "WorldRow exceeds the 32-field mask");
static_assert(std::tuple_size_v<decltype(PickRow::fields())> <= 32, "PickRow exceeds the 32-field mask");
static_assert(std::tuple_size_v<decltype(SpiritRow::fields())> <= 32, "SpiritRow exceeds the 32-field mask");

template <class Row, std::size_t Cap>
struct Section {    std::array<Row, Cap> rows;
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
    Section<SpiritRow, 32> sp;
    void clear() {
        l.clear(), e.clear(), ed.clear(), w.clear(), pk.clear(), sp.clear();
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
