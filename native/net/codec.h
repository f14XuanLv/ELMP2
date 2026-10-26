#pragma once

// Bounds-checked little-endian byte codec for the typed protocol (§8). The writer never allocates
// (caller-provided buffer) and latches `overflow`; the reader latches `bad` on any out-of-range read
// and then returns zeros — callers check `ok()` once at the end of a message (malformed input from the
// network never throws, never reads past the datagram).

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace elmp::net {

class Writer {
public:
    Writer(std::uint8_t* buf, std::size_t cap) : buf_(buf), cap_(cap) {}
    explicit Writer(std::span<std::uint8_t> s) : Writer(s.data(), s.size()) {}

    void u8(std::uint8_t v) { raw(&v, 1); }
    void u16(std::uint16_t v) { le(v); }
    void u32(std::uint32_t v) { le(v); }
    void u64(std::uint64_t v) { le(v); }
    void i8(std::int8_t v) { u8(static_cast<std::uint8_t>(v)); }
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float v) {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
    void var(std::uint32_t v) { // LEB128, ≤5 bytes
        do {
            std::uint8_t b = v & 0x7F;
            v >>= 7;
            u8(v ? b | 0x80 : b);
        } while (v);
    }
    void bytes(std::span<const std::uint8_t> b) { raw(b.data(), b.size()); }
    void str(std::string_view s) { // var length + utf8
        var(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void raw(const void* p, std::size_t n) {
        if (overflow_ || n > cap_ - pos_) {
            overflow_ = true;
            return;
        }
        if (n) std::memcpy(buf_ + pos_, p, n);
        pos_ += n;
    }
    // Reserve n bytes to patch later (e.g. a mask written after its fields); nullptr on overflow.
    std::uint8_t* reserve(std::size_t n) {
        if (overflow_ || n > cap_ - pos_) {
            overflow_ = true;
            return nullptr;
        }
        auto* p = buf_ + pos_;
        std::memset(p, 0, n);
        pos_ += n;
        return p;
    }
    std::size_t size() const { return pos_; }
    std::size_t room() const { return cap_ - pos_; }
    bool ok() const { return !overflow_; }
    void rewind(std::size_t pos) {
        if (pos <= pos_) pos_ = pos, overflow_ = false;
    }
    std::span<const std::uint8_t> view() const { return {buf_, pos_}; }

private:
    template <class T>
    void le(T v) {
        std::uint8_t b[sizeof(T)];
        for (std::size_t i = 0; i < sizeof(T); ++i) b[i] = static_cast<std::uint8_t>(v >> (8 * i));
        raw(b, sizeof(T));
    }
    std::uint8_t* buf_;
    std::size_t cap_, pos_ = 0;
    bool overflow_ = false;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> s) : p_(s.data()), n_(s.size()) {}

    std::uint8_t u8() {
        std::uint8_t v = 0;
        raw(&v, 1);
        return v;
    }
    std::uint16_t u16() { return le<std::uint16_t>(); }
    std::uint32_t u32() { return le<std::uint32_t>(); }
    std::uint64_t u64() { return le<std::uint64_t>(); }
    std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() {
        const auto u = u32();
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    }
    std::uint32_t var() {
        std::uint32_t v = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            const auto b = u8();
            if (bad_) return 0;
            if (shift == 28 && (b & 0xF0)) return fail(), 0; // > 32 bits
            v |= static_cast<std::uint32_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        return fail(), 0;
    }
    std::span<const std::uint8_t> bytes(std::size_t n) {
        if (bad_ || n > n_ - pos_) return fail(), std::span<const std::uint8_t>{};
        auto s = std::span<const std::uint8_t>(p_ + pos_, n);
        pos_ += n;
        return s;
    }
    std::string_view str(std::size_t max_len) {
        const auto n = var();
        if (n > max_len) return fail(), std::string_view{};
        const auto b = bytes(n);
        return {reinterpret_cast<const char*>(b.data()), b.size()};
    }
    void raw(void* out, std::size_t n) {
        if (bad_ || n > n_ - pos_) {
            fail();
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, p_ + pos_, n);
        pos_ += n;
    }
    bool ok() const { return !bad_; }
    bool done() const { return !bad_ && pos_ == n_; }
    std::size_t left() const { return bad_ ? 0 : n_ - pos_; }
    void fail() { bad_ = true; }

private:
    template <class T>
    T le() {
        std::uint8_t b[sizeof(T)]{};
        raw(b, sizeof(T));
        T v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(b[i]) << (8 * i));
        return v;
    }
    const std::uint8_t* p_;
    std::size_t n_, pos_ = 0;
    bool bad_ = false;
};

} // namespace elmp::net
