#pragma once

// One peer link of the session transport (C14 / FR-NET-5..8): IO-free state machine — the caller feeds
// datagrams + a monotonic clock and takes outbound datagrams + events. Sockets / threads live in
// net/endpoint. Semantics carried over from v1 (net-facts「传输选型」「安全加固记录」), re-implemented:
//   - handshake: ECDH P-256 ephemeral keys, HKDF-SHA256 with salt = SHA-256(normalised pairing code),
//     HMAC proofs both ways over the transcript (no code = no keys = no proof); identity (protocol / EXE /
//     main.dll / data hashes) checked by the host and rejected with a reason the joiner sees.
//   - data: AES-256-GCM per datagram, nonce = direction IV ‖ packet seq, 64-packet replay window.
//   - reliable ordered: segments ≤ 1100 B with per-segment seq; ACK = latest contiguous + 64-bit bitmap;
//     RTO = clamp(1.5 × SRTT, 100 ms, 2 s), ×1.5 back-off per resend (cap 2 s); 8 s without ACK progress
//     while data is outstanding = disconnect. Duplicate segments force an ACK (the ACK may be lost).
//   - unreliable latest-only per stream, fragmented at 1100 B, incomplete groups dropped.
//   - every queue bounded: reliable messages ≤ 256 pending, outbox ≤ 512 datagrams, reassembly groups
//     ≤ 32, events ≤ 512 (unreliable dropped first), PONG ≤ 4; reliable overload closes the link.
//     Terminal events supersede queued gameplay; F_CLOSE supersedes unsent datagrams.
//   - malformed input never throws (bad packets are counted); the link never allocates unboundedly.

#include "net/crypto.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace elmp::net {

using crypto::Bytes;

struct Identity {
    std::uint8_t proto = 0;
    std::array<std::uint8_t, 16> exe{}, dll{}, content{};
    bool operator==(const Identity&) const = default;
};

enum class Reject : std::uint8_t { None = 0, Proto = 1, Exe = 2, Dll = 3, Content = 4, Auth = 5, Full = 6 };
const char* reject_text(Reject r);

struct LinkStats {
    std::uint64_t tx_packets = 0, rx_packets = 0, tx_bytes = 0, rx_bytes = 0;
    std::uint64_t retransmits = 0, dup = 0, bad = 0, stale = 0, pong_dropped = 0, event_drops = 0, outbox_drops = 0;
    std::uint32_t unacked = 0, pending_msgs = 0;
    double rtt_ms = 0;
};

struct LinkEvent {
    enum class Kind : std::uint8_t { Connected, Rejected, Closed, Reliable, Unreliable };
    Kind kind;
    std::uint8_t stream = 0;
    Reject reason = Reject::None;
    Bytes data;
    std::string text;
};

class Link {
public:
    static constexpr std::size_t kMaxDatagram = 1200;
    static constexpr std::size_t kSegment = 1100;
    static constexpr std::size_t kMaxMessage = 128 * 1024; // protocol 4: 232 rows, 8 Spine tracks + Visual each
    static constexpr std::size_t kMaxPendingMsgs = 256;
    static constexpr std::size_t kMaxOutbox = 512;
    static constexpr std::size_t kMaxGroups = 32;
    static constexpr std::size_t kMaxEvents = 512;
    enum class Role : std::uint8_t { Host, Client };
    enum class Phase : std::uint8_t { Hello, Authing, Established, Closed };

    // Client: starts sending HELLO at once. Host: created by the endpoint for a new address; feed the HELLO.
    Link(Role role, const Identity& self, std::string_view pairing_code, double now);
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    Role role() const { return role_; }
    Phase phase() const { return phase_; }
    bool established() const { return phase_ == Phase::Established; }
    bool closed() const { return phase_ == Phase::Closed; }
    const Identity& peer_identity() const { return peer_id_; }

    void on_datagram(std::span<const std::uint8_t> pkt, double now);
    void poll(double now);
    void send_unreliable(std::uint8_t stream, std::span<const std::uint8_t> msg);
    bool send_reliable(std::span<const std::uint8_t> msg); // false: too large / not usable; queue full also closes
    void close(std::string_view reason);

    std::vector<Bytes> take_outbox();
    std::vector<LinkEvent> take_events();
    LinkStats stats() const;

    // Wire helpers for the endpoint (host side, before a link exists).
    static bool is_hello(std::span<const std::uint8_t> pkt);
    static Bytes reject_packet(Reject r);

private:
    struct Seg {
        std::uint32_t seq;
        Bytes data;
        bool last;
        double first_send = -1, last_send = -1;
        std::uint32_t sends = 0;
        bool acked = false;
    };
    struct Unrel {
        std::uint32_t seq = 0;
        Bytes data;
        std::uint8_t next_frag = 0, frags = 0;
        bool active = false;
    };
    struct Group {
        std::uint8_t stream;
        std::uint32_t seq;
        std::uint8_t count, got = 0;
        std::vector<Bytes> parts;
        double born;
    };

    void handshake_packet(std::uint8_t kind, std::span<const std::uint8_t> body);
    void on_handshake(std::uint8_t kind, std::span<const std::uint8_t> body, double now);
    bool derive(const crypto::Digest& z);
    void on_data(std::span<const std::uint8_t> pkt, double now);
    void on_frames(std::span<const std::uint8_t> frames, double now);
    void on_rel(std::uint32_t seq, bool last, std::span<const std::uint8_t> data);
    void on_ack(std::uint32_t base, std::uint64_t mask, double now);
    void on_unrel(std::uint8_t stream, std::uint32_t seq, std::uint8_t idx, std::uint8_t cnt, std::span<const std::uint8_t> data, double now);
    void emit(LinkEvent e);
    void fail(std::string text, Reject r = Reject::None, LinkEvent::Kind k = LinkEvent::Kind::Closed);
    void push_out(Bytes pkt);
    void seal_and_send(std::span<const std::uint8_t> frames);
    void pack(double now);
    double rto() const;

    Role role_;
    Identity self_{}, peer_id_{};
    crypto::Digest code_salt_{};
    Phase phase_ = Phase::Hello;

    crypto::Ecdh ecdh_;
    std::array<std::uint8_t, 16> nonce_{};
    Bytes hello_body_, ack_body_; // transcript material (ack body without the proof)
    crypto::Digest auth_key_{};
    std::array<std::uint8_t, 32> proof_host_{}, proof_client_{};
    crypto::AesGcm tx_, rx_;
    std::array<std::uint8_t, 4> tx_iv_{}, rx_iv_{};
    std::uint64_t sid_ = 0;
    double hs_deadline_ = 0, hs_retry_ = 0;
    Bytes hs_resend_; // last handshake datagram (resent until the next phase)

    std::uint64_t tx_seq_ = 0, rx_max_ = 0, rx_window_ = 0;
    double last_rx_ = 0, next_ping_ = 0, srtt_ = -1, last_progress_ = 0;

    std::deque<Seg> segs_;
    std::uint32_t rel_next_ = 1, acked_base_ = 0; // highest contiguous seq the peer has
    std::size_t pending_msgs_ = 0;
    std::uint32_t rx_expect_ = 1;
    std::map<std::uint32_t, std::pair<bool, Bytes>> rx_pending_;
    Bytes rx_msg_;
    bool ack_dirty_ = false, ack_force_ = false;
    std::uint32_t ack_sent_base_ = 0;
    std::uint64_t ack_sent_mask_ = 0;

    std::map<std::uint8_t, Unrel> unrel_;
    std::uint32_t unrel_seq_ = 1;
    std::map<std::uint8_t, std::uint32_t> unrel_rx_last_;
    std::vector<Group> groups_;
    std::deque<std::uint64_t> pongs_;

    std::vector<Bytes> outbox_;
    std::vector<LinkEvent> events_;
    LinkStats st_;
};

} // namespace elmp::net
