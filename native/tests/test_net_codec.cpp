// Typed codec boundaries (§13 CTest class 1): writer overflow latches, reader never reads past the end,
// varint limits, string caps, truncation of every prefix of a valid message fails cleanly.
#include "net/codec.h"

#include "tests/check.h"

#include <array>
#include <vector>

using namespace elmp::net;

TEST_MAIN({
    std::array<std::uint8_t, 64> buf{};
    Writer w(buf.data(), buf.size());
    w.u8(7);
    w.u16(0xBEEF);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFull);
    w.f32(1.5f);
    w.var(0);
    w.var(300);
    w.var(0xFFFFFFFFu);
    w.str("hi");
    CHECK(w.ok());
    const auto full = w.size();
    {
        Reader r(w.view());
        CHECK(r.u8() == 7);
        CHECK(r.u16() == 0xBEEF);
        CHECK(r.u32() == 0xDEADBEEF);
        CHECK(r.u64() == 0x0123456789ABCDEFull);
        CHECK(r.f32() == 1.5f);
        CHECK(r.var() == 0);
        CHECK(r.var() == 300);
        CHECK(r.var() == 0xFFFFFFFFu);
        CHECK(r.str(16) == "hi");
        CHECK(r.done());
    }
    for (std::size_t n = 0; n < full; ++n) { // every truncation fails without reading past n
        Reader r(std::span<const std::uint8_t>(buf.data(), n));
        r.u8(), r.u16(), r.u32(), r.u64(), r.f32(), r.var(), r.var(), r.var(), r.str(16);
        CHECK(!r.ok());
    }
    { // overflow latches and does not write past the capacity
        std::array<std::uint8_t, 8> small{};
        Writer s(small.data(), 4);
        s.u32(1);
        CHECK(s.ok());
        s.u8(9);
        CHECK(!s.ok());
        CHECK(small[4] == 0);
        CHECK(s.reserve(1) == nullptr);
    }
    { // varint longer than 5 bytes / > 32 bits rejected
        const std::uint8_t six[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
        Reader r(six);
        r.var();
        CHECK(!r.ok());
        const std::uint8_t big[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};
        Reader r2(big);
        r2.var();
        CHECK(!r2.ok());
    }
    { // string longer than the cap rejected; huge declared length rejected
        std::array<std::uint8_t, 32> b{};
        Writer s(b.data(), b.size());
        s.str("0123456789");
        Reader r(s.view());
        CHECK(r.str(4).empty());
        CHECK(!r.ok());
        const std::uint8_t lie[] = {0xFF, 0xFF, 0x03, 'a'};
        Reader r2(lie);
        r2.str(1 << 20);
        CHECK(!r2.ok());
    }
})
