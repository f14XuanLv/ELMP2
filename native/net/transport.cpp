#include "net/transport.h"

#include "net/codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace elmp::net {

namespace {

constexpr std::uint8_t kMagic0 = 'E', kMagic1 = 'L';
constexpr std::size_t kHeader = 20; // magic(2) proto(1) kind(1) sid(8) seq(8)
constexpr std::size_t kFrameRoom = Link::kMaxDatagram - kHeader - crypto::AesGcm::kTag;
enum Kind : std::uint8_t { K_HELLO = 1, K_HELLO_ACK = 2, K_AUTH = 3, K_REJECT = 4, K_DATA = 5 };
enum Frame : std::uint8_t { F_ACK = 1, F_REL = 2, F_UNREL = 3, F_PING = 4, F_PONG = 5, F_CLOSE = 6 };
constexpr std::size_t kIdentity = 1 + 48;
constexpr std::size_t kHelloBody = kIdentity + 16 + 65;
constexpr std::size_t kAckBody = 16 + 65; // + 32 proof on the wire
constexpr double kHsRetry = 0.25, kHsTimeout = 10.0, kPing = 1.0, kTimeout = 8.0, kStall = 8.0, kGroupAge = 2.0;
constexpr std::size_t kMaxPongs = 4, kPacketsPerPoll = 64, kWindow = 64;

std::string normalise(std::string_view code) {
    std::string s;
    for (char c : code) {
        if (c == ' ' || c == '-' || c == '\t') continue;
        s.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c);
    }
    return s;
}

void write_identity(Writer& w, const Identity& id) {
    w.u8(id.proto);
    w.raw(id.exe.data(), 16);
    w.raw(id.dll.data(), 16);
    w.raw(id.content.data(), 16);
}

Identity read_identity(Reader& r) {
    Identity id;
    id.proto = r.u8();
    r.raw(id.exe.data(), 16);
    r.raw(id.dll.data(), 16);
    r.raw(id.content.data(), 16);
    return id;
}

void header(Writer& w, std::uint8_t kind, std::uint64_t sid, std::uint64_t seq, std::uint8_t proto) {
    w.u8(kMagic0);
    w.u8(kMagic1);
    w.u8(proto);
    w.u8(kind);
    w.u64(sid);
    w.u64(seq);
}

std::string info(std::string_view label, const crypto::Digest& th) {
    std::string s(label);
    s.append(reinterpret_cast<const char*>(th.data()), th.size());
    return s;
}

} // namespace

const char* reject_text(Reject r) {
    switch (r) {
    case Reject::Proto: return "protocol version differs";
    case Reject::Exe: return "game executable differs";
    case Reject::Dll: return "ELMP main.dll differs";
    case Reject::Content: return "ELMP data differs";
    case Reject::Auth: return "pairing code mismatch";
    case Reject::Full: return "host is full";
    default: return "rejected";
    }
}

Link::Link(Role role, const Identity& self, std::string_view code, double now) : role_(role), self_(self) {
    const auto n = normalise(code);
    code_salt_ = crypto::sha256({reinterpret_cast<const std::uint8_t*>(n.data()), n.size()});
    crypto::random(nonce_);
    last_rx_ = last_progress_ = now;
    hs_deadline_ = now + kHsTimeout;
    if (!ecdh_.ok()) {
        phase_ = Phase::Closed;
        events_.push_back({LinkEvent::Kind::Closed, 0, Reject::None, {}, "crypto unavailable"});
        return;
    }
    if (role_ == Role::Client) {
        hello_body_.resize(kHelloBody);
        Writer w(hello_body_.data(), hello_body_.size());
        write_identity(w, self_);
        w.raw(nonce_.data(), 16);
        w.raw(ecdh_.pub().data(), 65);
        handshake_packet(K_HELLO, hello_body_);
        hs_retry_ = now + kHsRetry;
    }
}

Link::~Link() = default;

bool Link::is_hello(std::span<const std::uint8_t> p) { return p.size() == kHeader + kHelloBody && p[0] == kMagic0 && p[1] == kMagic1 && p[3] == K_HELLO; }

Bytes Link::reject_packet(Reject r) {
    Bytes b(kHeader + 1);
    Writer w(b.data(), b.size());
    header(w, K_REJECT, 0, 0, 0);
    w.u8(static_cast<std::uint8_t>(r));
    return b;
}

void Link::handshake_packet(std::uint8_t kind, std::span<const std::uint8_t> body) {
    Bytes b(kHeader + body.size());
    Writer w(b.data(), b.size());
    header(w, kind, 0, 0, self_.proto);
    w.bytes(body);
    hs_resend_ = b;
    push_out(std::move(b));
}

void Link::push_out(Bytes pkt) {
    if (outbox_.size() >= kMaxOutbox) {
        ++st_.outbox_drops;
        return;
    }
    ++st_.tx_packets;
    st_.tx_bytes += pkt.size();
    outbox_.push_back(std::move(pkt));
}

void Link::emit(LinkEvent e) {
    if (e.kind == LinkEvent::Kind::Closed || e.kind == LinkEvent::Kind::Rejected) {
        // Once closed, queued gameplay is stale. A terminal event must never be dropped or delayed.
        events_.clear();
        events_.push_back(std::move(e));
        return;
    }
    if (events_.size() >= kMaxEvents) {
        // Only latest-only data may be sacrificed while the link remains established.
        auto it = std::find_if(events_.begin(), events_.end(), [](const LinkEvent& x) { return x.kind == LinkEvent::Kind::Unreliable; });
        if (it == events_.end() && e.kind == LinkEvent::Kind::Unreliable) {
            ++st_.event_drops;
            return;
        }
        if (it == events_.end()) return close("reliable event queue overloaded");
        events_.erase(it);
        ++st_.event_drops;
    }
    events_.push_back(std::move(e));
}

void Link::fail(std::string text, Reject r, LinkEvent::Kind k) {
    if (phase_ == Phase::Closed) return;
    phase_ = Phase::Closed;
    emit({k, 0, r, {}, std::move(text)});
}

bool Link::derive(const crypto::Digest& z) {
    Bytes transcript = hello_body_;
    transcript.insert(transcript.end(), ack_body_.begin(), ack_body_.end());
    const auto th = crypto::sha256(transcript);
    const auto master = crypto::hkdf(z, code_salt_, info("ELMP v2 master", th), 32);
    const auto c2s = crypto::hkdf(master, {}, "c2s key", 32), s2c = crypto::hkdf(master, {}, "s2c key", 32);
    const auto c2s_iv = crypto::hkdf(master, {}, "c2s iv", 4), s2c_iv = crypto::hkdf(master, {}, "s2c iv", 4);
    const auto auth = crypto::hkdf(master, {}, "auth", 32), sid = crypto::hkdf(master, {}, "sid", 8);
    std::memcpy(auth_key_.data(), auth.data(), 32);
    std::memcpy(&sid_, sid.data(), 8);
    const bool host = role_ == Role::Host;
    tx_ = crypto::AesGcm(host ? s2c : c2s);
    rx_ = crypto::AesGcm(host ? c2s : s2c);
    std::memcpy(tx_iv_.data(), (host ? s2c_iv : c2s_iv).data(), 4);
    std::memcpy(rx_iv_.data(), (host ? c2s_iv : s2c_iv).data(), 4);
    auto proof = [&](std::string_view who) {
        Bytes m(who.begin(), who.end());
        m.insert(m.end(), transcript.begin(), transcript.end());
        return crypto::hmac_sha256(auth_key_, m);
    };
    proof_host_ = proof("host");
    proof_client_ = proof("client");
    return tx_.ok() && rx_.ok();
}

void Link::on_datagram(std::span<const std::uint8_t> pkt, double now) {
    if (phase_ == Phase::Closed) return;
    ++st_.rx_packets;
    st_.rx_bytes += pkt.size();
    if (pkt.size() < kHeader || pkt.size() > kMaxDatagram || pkt[0] != kMagic0 || pkt[1] != kMagic1) {
        ++st_.bad;
        return;
    }
    const auto kind = pkt[3];
    if (kind == K_DATA) return on_data(pkt, now);
    if (kind == K_REJECT) {
        if (role_ == Role::Client && phase_ != Phase::Established && pkt.size() == kHeader + 1) {
            const auto r = static_cast<Reject>(pkt[kHeader]);
            fail(reject_text(r), r, LinkEvent::Kind::Rejected);
        } else {
            ++st_.bad;
        }
        return;
    }
    on_handshake(kind, pkt.subspan(kHeader), now);
}

void Link::on_handshake(std::uint8_t kind, std::span<const std::uint8_t> body, double now) {
    if (role_ == Role::Host && kind == K_HELLO && body.size() == kHelloBody) {
        if (phase_ != Phase::Hello) { // client retry: resend our answer if it is the same hello
            if (crypto::equal_ct(body, hello_body_) && !hs_resend_.empty() && phase_ == Phase::Authing) push_out(hs_resend_);
            return;
        }
        Reader r(body);
        peer_id_ = read_identity(r);
        Reject rej = Reject::None;
        if (peer_id_.proto != self_.proto) rej = Reject::Proto;
        else if (peer_id_.exe != self_.exe) rej = Reject::Exe;
        else if (peer_id_.dll != self_.dll) rej = Reject::Dll;
        else if (peer_id_.content != self_.content) rej = Reject::Content;
        if (rej != Reject::None) {
            push_out(reject_packet(rej));
            return fail(reject_text(rej), rej, LinkEvent::Kind::Rejected);
        }
        r.bytes(16);
        const auto peer_pub = r.bytes(65);
        const auto z = ecdh_.shared(peer_pub); // untrusted point: nullopt on a bad point, never throws
        if (!z) {
            ++st_.bad;
            return;
        }
        hello_body_.assign(body.begin(), body.end());
        ack_body_.resize(kAckBody);
        Writer w(ack_body_.data(), ack_body_.size());
        w.raw(nonce_.data(), 16);
        w.raw(ecdh_.pub().data(), 65);
        if (!derive(*z)) return fail("crypto failure");
        Bytes wire = ack_body_;
        wire.insert(wire.end(), proof_host_.begin(), proof_host_.end());
        handshake_packet(K_HELLO_ACK, wire);
        phase_ = Phase::Authing;
        return;
    }
    if (role_ == Role::Client && kind == K_HELLO_ACK && body.size() == kAckBody + 32) {
        if (phase_ != Phase::Hello) return; // duplicate
        ack_body_.assign(body.begin(), body.begin() + kAckBody);
        const auto z = ecdh_.shared(body.subspan(16, 65));
        if (!z) {
            ++st_.bad;
            ack_body_.clear();
            return;
        }
        if (!derive(*z)) return fail("crypto failure");
        if (!crypto::equal_ct(body.subspan(kAckBody, 32), proof_host_)) return fail(reject_text(Reject::Auth), Reject::Auth, LinkEvent::Kind::Rejected);
        handshake_packet(K_AUTH, proof_client_);
        phase_ = Phase::Authing;
        hs_retry_ = now + kHsRetry;
        return;
    }
    if (role_ == Role::Host && kind == K_AUTH && body.size() == 32) {
        if (phase_ == Phase::Established) return; // client retry; our pings will reach it
        if (phase_ != Phase::Authing) {
            ++st_.bad;
            return;
        }
        if (!crypto::equal_ct(body, proof_client_)) {
            push_out(reject_packet(Reject::Auth));
            return fail(reject_text(Reject::Auth), Reject::Auth, LinkEvent::Kind::Rejected);
        }
        phase_ = Phase::Established;
        last_progress_ = now;
        next_ping_ = now; // the first encrypted packet tells the client we are established
        emit({LinkEvent::Kind::Connected});
        return;
    }
    ++st_.bad;
}

void Link::on_data(std::span<const std::uint8_t> pkt, double now) {
    if (phase_ == Phase::Hello || pkt.size() < kHeader + crypto::AesGcm::kTag) {
        ++st_.bad;
        return;
    }
    Reader h(pkt.subspan(0, kHeader));
    h.bytes(4);
    const auto sid = h.u64(), seq = h.u64();
    if (sid != sid_ || seq == 0) {
        ++st_.bad;
        return;
    }
    if (seq + kWindow <= rx_max_ || (seq <= rx_max_ && (rx_window_ >> (rx_max_ - seq)) & 1)) {
        ++st_.dup;
        return;
    }
    std::array<std::uint8_t, 12> nonce{};
    std::memcpy(nonce.data(), rx_iv_.data(), 4);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<std::uint8_t>(seq >> (8 * i));
    std::array<std::uint8_t, kMaxDatagram> plain;
    const auto body = pkt.subspan(kHeader);
    if (!rx_.open(nonce, pkt.subspan(0, kHeader), body, plain.data())) {
        ++st_.bad;
        return;
    }
    if (seq > rx_max_) {
        rx_window_ = seq - rx_max_ >= 64 ? 1 : (rx_window_ << (seq - rx_max_)) | 1;
        rx_max_ = seq;
    } else {
        rx_window_ |= 1ull << (rx_max_ - seq);
    }
    last_rx_ = now;
    if (phase_ == Phase::Authing && role_ == Role::Client) {
        phase_ = Phase::Established;
        last_progress_ = now;
        next_ping_ = now;
        emit({LinkEvent::Kind::Connected});
    }
    on_frames({plain.data(), body.size() - crypto::AesGcm::kTag}, now);
}

void Link::on_frames(std::span<const std::uint8_t> frames, double now) {
    Reader r(frames);
    while (r.left() && r.ok() && phase_ != Phase::Closed) {
        switch (r.u8()) {
        case F_ACK: {
            const auto base = r.u32();
            const auto mask = r.u64();
            if (r.ok()) on_ack(base, mask, now);
            break;
        }
        case F_REL: {
            const auto seq = r.u32();
            const bool last = r.u8() & 1;
            const auto data = r.bytes(r.u16());
            if (r.ok()) on_rel(seq, last, data);
            break;
        }
        case F_UNREL: {
            const auto stream = r.u8();
            const auto seq = r.u32();
            const auto idx = r.u8(), cnt = r.u8();
            const auto data = r.bytes(r.u16());
            if (r.ok()) on_unrel(stream, seq, idx, cnt, data, now);
            break;
        }
        case F_PING: {
            const auto t = r.u64();
            if (!r.ok()) break;
            if (pongs_.size() >= kMaxPongs) {
                pongs_.pop_front();
                ++st_.pong_dropped;
            }
            pongs_.push_back(t);
            break;
        }
        case F_PONG: {
            const auto t = r.u64();
            if (!r.ok()) break;
            const double sample = now - static_cast<double>(t) / 1e6;
            if (sample >= 0 && sample < 10) srtt_ = srtt_ < 0 ? sample : srtt_ * 0.875 + sample * 0.125;
            break;
        }
        case F_CLOSE: {
            const auto why = r.str(128);
            if (r.ok()) fail(std::string("peer closed: ") + std::string(why));
            return;
        }
        default: r.fail();
        }
    }
    if (!r.ok()) ++st_.bad;
}

void Link::on_rel(std::uint32_t seq, bool last, std::span<const std::uint8_t> data) {
    ack_dirty_ = true;
    if (seq < rx_expect_ || rx_pending_.count(seq)) { // duplicate: our ACK may have been lost
        ++st_.dup;
        ack_force_ = true;
        return;
    }
    if (seq >= rx_expect_ + kWindow) { // outside the window the sender may use
        ++st_.bad;
        return;
    }
    rx_pending_[seq] = {last, Bytes(data.begin(), data.end())};
    for (auto it = rx_pending_.find(rx_expect_); it != rx_pending_.end(); it = rx_pending_.find(rx_expect_)) {
        auto& [is_last, bytes] = it->second;
        if (rx_msg_.size() + bytes.size() > kMaxMessage) return fail("reliable message too large");
        rx_msg_.insert(rx_msg_.end(), bytes.begin(), bytes.end());
        if (is_last) {
            emit({LinkEvent::Kind::Reliable, 0, Reject::None, std::move(rx_msg_)});
            rx_msg_.clear();
            if (closed()) return;
        }
        rx_pending_.erase(it);
        ++rx_expect_;
    }
}

void Link::on_ack(std::uint32_t base, std::uint64_t mask, double now) {
    bool progress = false;
    for (auto& s : segs_) {
        if (s.acked) continue;
        if (s.seq <= base || (s.seq > base && s.seq - base - 1 < 64 && ((mask >> (s.seq - base - 1)) & 1))) {
            s.acked = true;
            progress = true;
            if (s.sends == 1 && s.first_send >= 0) {
                const double sample = now - s.first_send; // Karn: only never-resent segments
                srtt_ = srtt_ < 0 ? sample : srtt_ * 0.875 + sample * 0.125;
            }
        }
    }
    while (!segs_.empty() && segs_.front().acked) {
        if (segs_.front().last && pending_msgs_) --pending_msgs_;
        acked_base_ = segs_.front().seq;
        segs_.pop_front();
    }
    if (progress) last_progress_ = now;
}

void Link::on_unrel(std::uint8_t stream, std::uint32_t seq, std::uint8_t idx, std::uint8_t cnt, std::span<const std::uint8_t> data, double now) {
    if (cnt == 0 || idx >= cnt || cnt > (kMaxMessage + kSegment - 1) / kSegment || data.size() > kSegment) {
        ++st_.bad;
        return;
    }
    auto last = unrel_rx_last_.find(stream);
    if (last != unrel_rx_last_.end() && seq <= last->second) {
        ++st_.stale;
        return;
    }
    auto deliver = [&](Bytes msg) {
        unrel_rx_last_[stream] = seq;
        std::erase_if(groups_, [&](const Group& g) { return g.stream == stream && g.seq <= seq; });
        emit({LinkEvent::Kind::Unreliable, stream, Reject::None, std::move(msg)});
    };
    if (cnt == 1) return deliver(Bytes(data.begin(), data.end()));
    auto g = std::find_if(groups_.begin(), groups_.end(), [&](const Group& x) { return x.stream == stream && x.seq == seq; });
    if (g == groups_.end()) {
        std::erase_if(groups_, [&](const Group& x) { return now - x.born > kGroupAge; });
        if (groups_.size() >= kMaxGroups) {
            ++st_.bad;
            return;
        }
        groups_.push_back({stream, seq, cnt, 0, std::vector<Bytes>(cnt), now});
        g = groups_.end() - 1;
    }
    if (g->count != cnt || !g->parts[idx].empty() || data.empty()) {
        ++st_.dup;
        return;
    }
    g->parts[idx].assign(data.begin(), data.end());
    if (++g->got < g->count) return;
    Bytes msg;
    for (auto& p : g->parts) {
        if (p.size() > kMaxMessage - msg.size()) { groups_.erase(g); ++st_.bad; return; }
        msg.insert(msg.end(), p.begin(), p.end());
    }
    deliver(std::move(msg));
}

void Link::send_unreliable(std::uint8_t stream, std::span<const std::uint8_t> msg) {
    if (phase_ != Phase::Established || msg.empty() || msg.size() > kMaxMessage) return;
    auto& u = unrel_[stream];
    if (u.active) ++st_.stale; // replaced before fully sent: latest wins
    u.seq = unrel_seq_++;
    u.data.assign(msg.begin(), msg.end());
    u.next_frag = 0;
    u.frags = static_cast<std::uint8_t>((msg.size() + kSegment - 1) / kSegment);
    u.active = true;
}

bool Link::send_reliable(std::span<const std::uint8_t> msg) {
    if (phase_ == Phase::Closed || phase_ == Phase::Hello || msg.empty() || msg.size() > kMaxMessage) return false;
    if (pending_msgs_ >= kMaxPendingMsgs) {
        close("reliable send queue overloaded");
        return false;
    }
    for (std::size_t off = 0; off < msg.size(); off += kSegment) {
        const auto n = std::min(kSegment, msg.size() - off);
        segs_.push_back({rel_next_++, Bytes(msg.begin() + static_cast<std::ptrdiff_t>(off), msg.begin() + static_cast<std::ptrdiff_t>(off + n)), off + n == msg.size()});
    }
    ++pending_msgs_;
    return true;
}

void Link::close(std::string_view reason) {
    if (phase_ == Phase::Closed) return;
    outbox_.clear(); // never let queued data crowd out F_CLOSE or precede it after local shutdown
    if (phase_ == Phase::Established) {
        std::array<std::uint8_t, 160> f;
        Writer w(f.data(), f.size());
        w.u8(F_CLOSE);
        w.str(reason.substr(0, 128));
        seal_and_send(w.view());
        seal_and_send(w.view()); // one loss must not leave the peer waiting 8 s
    }
    fail(std::string("closed: ") + std::string(reason));
}

double Link::rto() const { return std::clamp(srtt_ < 0 ? 0.2 : srtt_ * 1.5, 0.1, 2.0); }

void Link::seal_and_send(std::span<const std::uint8_t> frames) {
    Bytes b(kHeader + frames.size() + crypto::AesGcm::kTag);
    Writer w(b.data(), b.size());
    const auto seq = ++tx_seq_;
    header(w, K_DATA, sid_, seq, self_.proto);
    std::array<std::uint8_t, 12> nonce{};
    std::memcpy(nonce.data(), tx_iv_.data(), 4);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<std::uint8_t>(seq >> (8 * i));
    if (!tx_.seal(nonce, std::span<const std::uint8_t>(b.data(), kHeader), frames, b.data() + kHeader)) return;
    push_out(std::move(b));
}

void Link::poll(double now) {
    if (phase_ == Phase::Closed) return;
    if (phase_ != Phase::Established) {
        if (now > hs_deadline_) return fail("handshake timeout");
        if (role_ == Role::Client && now >= hs_retry_ && !hs_resend_.empty()) {
            push_out(hs_resend_);
            hs_retry_ = now + kHsRetry;
        }
        return;
    }
    if (now - last_rx_ > kTimeout) return fail("connection timed out");
    const bool outstanding = std::any_of(segs_.begin(), segs_.end(), [](const Seg& s) { return s.sends && !s.acked; });
    if (outstanding && now - last_progress_ > kStall) return fail("reliable channel stalled");
    if (!outstanding) last_progress_ = now;
    pack(now);
}

void Link::pack(double now) {
    std::uint32_t base = rx_expect_ - 1;
    std::uint64_t mask = 0;
    for (const auto& [seq, _] : rx_pending_)
        if (seq > base && seq - base - 1 < 64) mask |= 1ull << (seq - base - 1);
    bool want_ack = ack_force_ || (ack_dirty_ && (base != ack_sent_base_ || mask != ack_sent_mask_));
    bool want_ping = now >= next_ping_;
    std::array<std::uint8_t, kFrameRoom> buf;
    for (std::size_t packets = 0; packets < kPacketsPerPoll; ++packets) {
        Writer w(buf.data(), buf.size());
        if (want_ack) {
            w.u8(F_ACK);
            w.u32(base);
            w.u64(mask);
            ack_sent_base_ = base, ack_sent_mask_ = mask;
            want_ack = ack_force_ = ack_dirty_ = false;
        }
        while (!pongs_.empty() && w.room() >= 9) {
            w.u8(F_PONG);
            w.u64(pongs_.front());
            pongs_.pop_front();
        }
        if (want_ping && w.room() >= 9) {
            w.u8(F_PING);
            w.u64(static_cast<std::uint64_t>(now * 1e6));
            want_ping = false;
            next_ping_ = now + kPing;
        }
        // reliable: due segments within the 64-segment window above the acked base
        for (auto& s : segs_) {
            if (s.acked) continue;
            if (s.seq > acked_base_ + kWindow) break;
            const bool due = s.sends == 0 || now - s.last_send >= std::min(2.0, rto() * std::pow(1.5, static_cast<double>(s.sends - 1)));
            if (!due) continue;
            if (w.room() < 8 + s.data.size()) break;
            w.u8(F_REL);
            w.u32(s.seq);
            w.u8(s.last ? 1 : 0);
            w.u16(static_cast<std::uint16_t>(s.data.size()));
            w.bytes(s.data);
            if (s.sends) ++st_.retransmits;
            if (s.first_send < 0) s.first_send = now;
            s.last_send = now;
            ++s.sends;
        }
        for (auto& [stream, u] : unrel_) {
            while (u.active) {
                const std::size_t off = static_cast<std::size_t>(u.next_frag) * kSegment;
                const auto n = std::min(kSegment, u.data.size() - off);
                if (w.room() < 11 + n) break;
                w.u8(F_UNREL);
                w.u8(stream);
                w.u32(u.seq);
                w.u8(u.next_frag);
                w.u8(u.frags);
                w.u16(static_cast<std::uint16_t>(n));
                w.raw(u.data.data() + off, n);
                if (++u.next_frag >= u.frags) u.active = false;
            }
        }
        if (w.size() == 0) break;
        seal_and_send(w.view());
    }
}

std::vector<Bytes> Link::take_outbox() { return std::exchange(outbox_, {}); }
std::vector<LinkEvent> Link::take_events() { return std::exchange(events_, {}); }

LinkStats Link::stats() const {
    auto s = st_;
    s.unacked = static_cast<std::uint32_t>(std::count_if(segs_.begin(), segs_.end(), [](const Seg& x) { return x.sends && !x.acked; }));
    s.pending_msgs = static_cast<std::uint32_t>(pending_msgs_);
    s.rtt_ms = srtt_ < 0 ? 0 : srtt_ * 1000;
    return s;
}

} // namespace elmp::net
