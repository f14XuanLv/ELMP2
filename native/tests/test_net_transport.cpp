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
    { // queue bounds: 256 pending messages, message size cap
        Net n;
        n.step(20);
        n.cut = true;
        int ok = 0;
        for (int i = 0; i < 400; ++i) ok += n.a.send_reliable(msg(i, 10));
        CHECK(ok == 256);
        CHECK(!n.a.send_reliable(Bytes(Link::kMaxMessage + 1)));
        n.step(200, 0.05); // 10 s cut: stall / timeout closes the link
        CHECK(n.a.closed());
    }
    { // F_CLOSE reaches the peer immediately
        Net n;
        n.step(20);
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
