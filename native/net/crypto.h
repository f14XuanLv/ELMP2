#pragma once

// Windows CNG primitives for the session transport (§8, C14): SHA-256, HMAC-SHA256, HKDF-SHA256,
// ECDH P-256, AES-256-GCM, system RNG. Pure base layer (no UE4SS), loop-back testable.
// The pairing code only enters through HKDF salt + HMAC proofs (friend-pairing threat model, v1
// net-facts「传输选型」).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace elmp::net::crypto {

using Bytes = std::vector<std::uint8_t>;
using Digest = std::array<std::uint8_t, 32>;

Digest sha256(std::span<const std::uint8_t> data);
Digest hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);
Bytes hkdf(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt, std::string_view info, std::size_t out_len);
void random(std::span<std::uint8_t> out);
bool equal_ct(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b); // constant-time

// Ephemeral ECDH P-256 key; pub = 65-byte uncompressed point (04 || X || Y).
class Ecdh {
public:
    Ecdh(); // generates; ok() false when CNG failed
    ~Ecdh();
    Ecdh(const Ecdh&) = delete;
    Ecdh& operator=(const Ecdh&) = delete;
    bool ok() const { return key_ != nullptr; }
    const std::array<std::uint8_t, 65>& pub() const { return pub_; }
    // 32-byte shared secret (raw X coordinate hashed by CNG "SHA256" KDF); nullopt for a bad point.
    std::optional<Digest> shared(std::span<const std::uint8_t> peer_pub) const;

private:
    void* key_ = nullptr;
    std::array<std::uint8_t, 65> pub_{};
};

// AES-256-GCM; nonce 12 bytes never reused under one key (the caller derives it from a packet seq).
class AesGcm {
public:
    static constexpr std::size_t kTag = 16;
    AesGcm() = default;
    explicit AesGcm(std::span<const std::uint8_t> key32);
    ~AesGcm();
    AesGcm(AesGcm&& o) noexcept : key_(o.key_) { o.key_ = nullptr; }
    AesGcm& operator=(AesGcm&& o) noexcept;
    AesGcm(const AesGcm&) = delete;
    AesGcm& operator=(const AesGcm&) = delete;
    bool ok() const { return key_ != nullptr; }
    // out = cipher || tag; out must hold plain.size() + kTag bytes. False on CNG failure.
    bool seal(std::span<const std::uint8_t> nonce12, std::span<const std::uint8_t> aad, std::span<const std::uint8_t> plain, std::uint8_t* out) const;
    // out must hold cipher.size() - kTag bytes. False on authentication failure.
    bool open(std::span<const std::uint8_t> nonce12, std::span<const std::uint8_t> aad, std::span<const std::uint8_t> cipher_tag, std::uint8_t* out) const;

private:
    void* key_ = nullptr;
};

} // namespace elmp::net::crypto
