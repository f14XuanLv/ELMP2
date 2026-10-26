#include "net/crypto.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace elmp::net::crypto {

namespace {

bool nt_ok(NTSTATUS s) { return s >= 0; }

BCRYPT_ALG_HANDLE open(const wchar_t* alg, ULONG flags = 0) {
    BCRYPT_ALG_HANDLE h{};
    return nt_ok(BCryptOpenAlgorithmProvider(&h, alg, nullptr, flags)) ? h : nullptr;
}

BCRYPT_ALG_HANDLE sha_alg() {
    static BCRYPT_ALG_HANDLE h = open(BCRYPT_SHA256_ALGORITHM);
    return h;
}
BCRYPT_ALG_HANDLE hmac_alg() {
    static BCRYPT_ALG_HANDLE h = open(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    return h;
}
BCRYPT_ALG_HANDLE ecdh_alg() {
    static BCRYPT_ALG_HANDLE h = open(BCRYPT_ECDH_P256_ALGORITHM);
    return h;
}
BCRYPT_ALG_HANDLE gcm_alg() {
    static BCRYPT_ALG_HANDLE h = [] {
        auto a = open(BCRYPT_AES_ALGORITHM);
        if (a && !nt_ok(BCryptSetProperty(a, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                       sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
            BCryptCloseAlgorithmProvider(a, 0);
            a = nullptr;
        }
        return a;
    }();
    return h;
}

PUCHAR mut(const std::uint8_t* p) { return const_cast<PUCHAR>(p); }

} // namespace

Digest sha256(std::span<const std::uint8_t> data) {
    Digest d{};
    BCryptHash(sha_alg(), nullptr, 0, mut(data.data()), static_cast<ULONG>(data.size()), d.data(), static_cast<ULONG>(d.size()));
    return d;
}

Digest hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
    Digest d{};
    static const std::uint8_t zero = 0;
    BCryptHash(hmac_alg(), key.empty() ? mut(&zero) : mut(key.data()), static_cast<ULONG>(key.size()), mut(data.data()), static_cast<ULONG>(data.size()), d.data(),
               static_cast<ULONG>(d.size()));
    return d;
}

Bytes hkdf(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt, std::string_view info, std::size_t out_len) {
    // RFC 5869 extract + expand
    Digest zero_salt{};
    const auto prk = hmac_sha256(salt.empty() ? std::span<const std::uint8_t>(zero_salt) : salt, ikm);
    Bytes out;
    out.reserve(out_len);
    Bytes block;
    Digest t{};
    std::size_t tlen = 0;
    for (std::uint8_t i = 1; out.size() < out_len; ++i) {
        block.assign(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(tlen));
        block.insert(block.end(), info.begin(), info.end());
        block.push_back(i);
        t = hmac_sha256(prk, block);
        tlen = t.size();
        const auto take = std::min(t.size(), out_len - out.size());
        out.insert(out.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return out;
}

void random(std::span<std::uint8_t> out) {
    BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

bool equal_ct(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    if (a.size() != b.size()) return false;
    std::uint8_t d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) d |= a[i] ^ b[i];
    return d == 0;
}

Ecdh::Ecdh() {
    auto alg = ecdh_alg();
    BCRYPT_KEY_HANDLE k{};
    if (!alg || !nt_ok(BCryptGenerateKeyPair(alg, &k, 256, 0)) || !nt_ok(BCryptFinalizeKeyPair(k, 0))) {
        if (k) BCryptDestroyKey(k);
        return;
    }
    std::uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64]{};
    ULONG n = 0;
    if (!nt_ok(BCryptExportKey(k, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &n, 0)) || n != sizeof(blob)) {
        BCryptDestroyKey(k);
        return;
    }
    pub_[0] = 0x04;
    std::memcpy(pub_.data() + 1, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    key_ = k;
}

Ecdh::~Ecdh() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

std::optional<Digest> Ecdh::shared(std::span<const std::uint8_t> peer_pub) const {
    if (!key_ || peer_pub.size() != 65 || peer_pub[0] != 0x04) return std::nullopt;
    std::uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64]{};
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    hdr->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), peer_pub.data() + 1, 64);
    BCRYPT_KEY_HANDLE peer{};
    if (!nt_ok(BCryptImportKeyPair(ecdh_alg(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer, blob, sizeof(blob), 0))) return std::nullopt;
    BCRYPT_SECRET_HANDLE secret{};
    std::optional<Digest> out;
    if (nt_ok(BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(key_), peer, &secret, 0))) {
        BCryptBuffer buf{static_cast<ULONG>((wcslen(BCRYPT_SHA256_ALGORITHM) + 1) * sizeof(wchar_t)), KDF_HASH_ALGORITHM,
                         const_cast<wchar_t*>(BCRYPT_SHA256_ALGORITHM)};
        BCryptBufferDesc desc{BCRYPTBUFFER_VERSION, 1, &buf};
        Digest d{};
        ULONG n = 0;
        if (nt_ok(BCryptDeriveKey(secret, BCRYPT_KDF_HASH, &desc, d.data(), static_cast<ULONG>(d.size()), &n, 0)) && n == d.size()) out = d;
        BCryptDestroySecret(secret);
    }
    BCryptDestroyKey(peer);
    return out;
}

AesGcm::AesGcm(std::span<const std::uint8_t> key32) {
    BCRYPT_KEY_HANDLE k{};
    if (key32.size() == 32 && gcm_alg() && nt_ok(BCryptGenerateSymmetricKey(gcm_alg(), &k, nullptr, 0, mut(key32.data()), 32, 0))) key_ = k;
}

AesGcm::~AesGcm() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

AesGcm& AesGcm::operator=(AesGcm&& o) noexcept {
    if (this != &o) {
        if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
        key_ = o.key_;
        o.key_ = nullptr;
    }
    return *this;
}

bool AesGcm::seal(std::span<const std::uint8_t> nonce12, std::span<const std::uint8_t> aad, std::span<const std::uint8_t> plain, std::uint8_t* out) const {
    if (!key_ || nonce12.size() != 12) return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = mut(nonce12.data());
    info.cbNonce = 12;
    info.pbAuthData = mut(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = out + plain.size();
    info.cbTag = kTag;
    ULONG n = 0;
    static std::uint8_t empty = 0;
    return nt_ok(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), plain.empty() ? &empty : mut(plain.data()), static_cast<ULONG>(plain.size()), &info, nullptr, 0,
                            plain.empty() ? &empty : out, static_cast<ULONG>(plain.size()), &n, 0)) &&
           n == plain.size();
}

bool AesGcm::open(std::span<const std::uint8_t> nonce12, std::span<const std::uint8_t> aad, std::span<const std::uint8_t> cipher_tag, std::uint8_t* out) const {
    if (!key_ || nonce12.size() != 12 || cipher_tag.size() < kTag) return false;
    const auto n_plain = cipher_tag.size() - kTag;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = mut(nonce12.data());
    info.cbNonce = 12;
    info.pbAuthData = mut(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = mut(cipher_tag.data() + n_plain);
    info.cbTag = kTag;
    ULONG n = 0;
    static std::uint8_t empty = 0;
    return nt_ok(BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), n_plain ? mut(cipher_tag.data()) : &empty, static_cast<ULONG>(n_plain), &info, nullptr, 0,
                            n_plain ? out : &empty, static_cast<ULONG>(n_plain), &n, 0)) &&
           n == n_plain;
}

} // namespace elmp::net::crypto
