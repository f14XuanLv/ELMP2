// Transport state machine (§13 CTest class 2): reliable ordered delivery under loss / reorder /
// duplication, fragmentation, latest-only unreliable, bounded queues, stall + timeout, F_CLOSE.
#include "net/transport.h"

#include "tests/check.h"

#include <algorithm>
#include <random>
#include <vector>

using namespace elmp::net;

namespace {

Identity ident() {
    Identity i;
    i.proto = 2;
    return i;
}

struct Net {
    Link a{Link::Role::Host, ident(), "k", 0}, b{Link::Role::Client, ident(), "k", 0};
    double now = 0;
    double loss = 0, dup = 0;
    bool reorder = false, cut = false;
    std::mt19937 rng{7};
    std::vector<LinkEvent> ea, eb;
    void hop(Link& from, Link& to) {
        auto out = from.take_outbox();
        if (reorder) std::shuffle(out.begin(), out.end(), rng);
        std::uniform_real_distribution<double> u(0, 1);
        for (auto& d : out) {
            if (cut || u(rng) < loss) continue;
            to.on_datagram(d, now);
            if (u(rng) < dup) to.on_datagram(d, now);
        }
    }
    void step(int n, double dt = 0.01) {
        for (int i = 0; i < n; ++i) {
            now += dt;
            hop(b, a);
            hop(a, b);
            a.poll(now);
            b.poll(now);
            for (auto& e : a.take_events()) ea.push_back(std::move(e));
            for (auto& e : b.take_events()) eb.push_back(std::move(e));
        }
    }
    static std::vector<Bytes> rel(const std::vector<LinkEvent>& v) {
        std::vector<Bytes> r;
        for (auto& e : v)
            if (e.kind == LinkEvent::Kind::Reliable) r.push_back(e.data);
        return r;
    }
};

Bytes msg(std::uint32_t i, std::size_t n) {
    Bytes b(n);
    for (std::size_t k = 0; k < n; ++k) b[k] = static_cast<std::uint8_t>(i * 31 + k);
    return b;
}

} // namespace

TEST_MAIN({
    { // ordered, exactly-once under 20% loss + duplication + reordering, with multi-segment messages
        Net n;
        n.step(20);
        CHECK(n.a.established() && n.b.established());
        n.loss = 0.2, n.dup = 0.2, n.reorder = true;
        std::vector<Bytes> sent;
        for (std::uint32_t i = 0; i < 120; ++i) {
            sent.push_back(msg(i, i % 10 == 0 ? 5000 : 40 + i));
            CHECK(n.a.send_reliable(sent.back()));
        }
        n.step(3000);
        const auto got = Net::rel(n.eb);
        CHECK(got == sent);
        CHECK(n.a.stats().retransmits > 0);
        CHECK(n.b.stats().dup > 0);
        CHECK(n.a.established());
    }
    { // unreliable: latest only, fragmented message reassembled, stale dropped
        Net n;
        n.step(20);
        for (std::uint8_t i = 0; i < 5; ++i) n.a.send_unreliable(1, msg(i, 3000)); // replaced before sending
        n.step(5);
        int count = 0;
        for (auto& e : n.eb)
            if (e.kind == LinkEvent::Kind::Unreliable) {
                ++count;
                CHECK(e.data == msg(4, 3000));
            }
        CHECK(count == 1);
    }
    { // A final reliable lifecycle event cannot be rejected while retaining an active session.
        Net n;
        n.step(20);
        for (std::size_t i = 0; i < Link::kMaxPendingMsgs; ++i) CHECK(n.a.send_reliable(msg(static_cast<std::uint32_t>(i), 10)));
        CHECK(n.a.established());
        CHECK(!n.a.send_reliable(msg(257, 10))); // the would-be final Stop exceeds the pending limit
        CHECK(n.a.closed());
        CHECK(!n.a.send_reliable(msg(258, 10)));
        n.a.close("again");
        n.step(3);
        CHECK(n.b.closed());
        for (const auto* events : {&n.ea, &n.eb}) {
            int closed = 0;
            for (const auto& e : *events) if (e.kind == LinkEvent::Kind::Closed) {
                ++closed;
                CHECK(e.text.find("reliable send queue overloaded") != std::string::npos);
            }
            CHECK(closed == 1);
        }
        CHECK(Net::rel(n.eb).empty());
    }
    { // A stalled event consumer must observe one terminal event, never a reliable sequence gap.
        Net n;
        n.step(20);
        for (std::size_t i = 0; i <= Link::kMaxEvents; ++i) {
            CHECK(n.a.send_reliable(msg(static_cast<std::uint32_t>(i), 10)));
            n.now += 0.001;
            n.a.poll(n.now);
            n.hop(n.a, n.b);
            n.b.poll(n.now);
            n.hop(n.b, n.a); // ACKs progress; b's application intentionally does not drain its events
        }
        CHECK(n.a.closed() && n.b.closed());
        const auto terminal = n.b.take_events();
        CHECK(terminal.size() == 1 && terminal[0].kind == LinkEvent::Kind::Closed);
        CHECK(terminal[0].text.find("reliable event queue overloaded") != std::string::npos);
        n.step(3);
        CHECK(n.b.take_events().empty());
    }
    { // size rejection is nonfatal, but missing ACK progress still closes within the stall timeout
        Net n;
        n.step(20);
        CHECK(!n.a.send_reliable(Bytes(Link::kMaxMessage + 1)));
        CHECK(n.a.established());
        n.cut = true;
        CHECK(n.a.send_reliable(msg(1, 10)));
        n.step(200, 0.05); // 10 s cut: stall / timeout closes the link
        CHECK(n.a.closed());
    }
    { // Protocol 4 full pose snapshots need >64 KiB; the exact cap has a short final fragment.
        Net n; n.step(20); n.reorder = true; n.dup = 0.2;
        const auto large = msg(17, Link::kMaxMessage);
        n.a.send_unreliable(2, large);
        n.step(12);
        int count = 0;
        for (const auto& e : n.eb) if (e.kind == LinkEvent::Kind::Unreliable) { ++count; CHECK(e.data == large); }
        CHECK(count == 1 && n.a.established() && n.b.established());
    }
    { // F_CLOSE reaches the peer immediately even when unsent datagrams fill the outbox.
        Net n;
        n.step(20);
        for (std::size_t i = 0; i <= Link::kMaxOutbox; ++i) {
            n.a.send_unreliable(1, msg(static_cast<std::uint32_t>(i), 1000));
            n.a.poll(n.now += 0.0001); // deliberately do not drain the sender outbox
        }
        CHECK(n.a.stats().outbox_drops > 0);
        n.a.close("bye");
        n.step(3);
        CHECK(n.b.closed());
        bool reason = false;
        for (auto& e : n.eb) reason |= e.kind == LinkEvent::Kind::Closed && e.text.find("bye") != std::string::npos;
        CHECK(reason);
    }
    { // idle link stays up (pings), and outbox never exceeds the cap
        Net n;
        n.step(20);
        n.step(1000, 0.02);
        CHECK(n.a.established() && n.b.established());
        CHECK(n.a.stats().rtt_ms >= 0);
        CHECK(n.a.stats().outbox_drops == 0);
    }
})
