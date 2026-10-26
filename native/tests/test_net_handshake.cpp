// CNG handshake (§13 CTest class 3): good pairing code connects; wrong code / version / EXE / data hash
// are rejected with the reason reaching the joiner; garbage and bad EC points never throw or connect.
#include "net/transport.h"

#include "tests/check.h"

#include <vector>

using namespace elmp::net;

namespace {

Identity ident(std::uint8_t proto = 2, std::uint8_t exe = 1) {
    Identity i;
    i.proto = proto;
    i.exe.fill(exe);
    i.dll.fill(2);
    i.content.fill(3);
    return i;
}

struct Pair {
    Link host, client;
    double now = 0;
    std::vector<LinkEvent> he, ce;
    Pair(const Identity& h, const Identity& c, const char* hc, const char* cc) : host(Link::Role::Host, h, hc, 0), client(Link::Role::Client, c, cc, 0) {}
    void run(int steps) {
        for (int i = 0; i < steps; ++i) {
            now += 0.01;
            for (auto& d : client.take_outbox()) host.on_datagram(d, now);
            for (auto& d : host.take_outbox()) client.on_datagram(d, now);
            host.poll(now);
            client.poll(now);
            for (auto& e : host.take_events()) he.push_back(std::move(e));
            for (auto& e : client.take_events()) ce.push_back(std::move(e));
        }
    }
    static bool has(const std::vector<LinkEvent>& v, LinkEvent::Kind k, Reject r = Reject::None) {
        for (auto& e : v)
            if (e.kind == k && (r == Reject::None || e.reason == r)) return true;
        return false;
    }
};

} // namespace

TEST_MAIN({
    { // good code (normalisation: case / dashes / spaces)
        Pair p(ident(), ident(), "ab-cd 12", "ABCD12");
        p.run(20);
        CHECK(p.host.established());
        CHECK(p.client.established());
        CHECK(Pair::has(p.he, LinkEvent::Kind::Connected));
        CHECK(Pair::has(p.ce, LinkEvent::Kind::Connected));
        CHECK(p.client.send_reliable(std::vector<std::uint8_t>{1, 2, 3}));
        p.run(10);
        CHECK(Pair::has(p.he, LinkEvent::Kind::Reliable));
    }
    { // wrong code: client detects the host proof mismatch
        Pair p(ident(), ident(), "1111", "2222");
        p.run(20);
        CHECK(!p.host.established());
        CHECK(!p.client.established());
        CHECK(Pair::has(p.ce, LinkEvent::Kind::Rejected, Reject::Auth));
    }
    { // version / exe / content mismatch → reason reaches the client
        Pair a(ident(2), ident(3), "x", "x");
        a.run(10);
        CHECK(Pair::has(a.ce, LinkEvent::Kind::Rejected, Reject::Proto));
        Pair b(ident(2, 1), ident(2, 9), "x", "x");
        b.run(10);
        CHECK(Pair::has(b.ce, LinkEvent::Kind::Rejected, Reject::Exe));
        Identity c = ident();
        c.content.fill(7);
        Pair d(ident(), c, "x", "x");
        d.run(10);
        CHECK(Pair::has(d.ce, LinkEvent::Kind::Rejected, Reject::Content));
    }
    { // garbage, truncated, bad EC point: counted, no connect, no throw
        Link host(Link::Role::Host, ident(), "x", 0);
        Link client(Link::Role::Client, ident(), "x", 0);
        auto hello = client.take_outbox().at(0);
        std::vector<std::uint8_t> junk(300, 0xAB);
        host.on_datagram(junk, 0.1);
        host.on_datagram(std::span(hello).first(30), 0.1);
        auto bad = hello;
        for (std::size_t i = bad.size() - 64; i < bad.size(); ++i) bad[i] = 0x11; // point not on the curve
        host.on_datagram(bad, 0.1);
        CHECK(host.phase() == Link::Phase::Hello);
        CHECK(host.stats().bad >= 2);
        host.on_datagram(hello, 0.2); // a good hello still works afterwards
        CHECK(host.phase() == Link::Phase::Authing);
    }
    { // handshake timeout
        Link client(Link::Role::Client, ident(), "x", 0);
        client.poll(11);
        CHECK(client.closed());
    }
})
