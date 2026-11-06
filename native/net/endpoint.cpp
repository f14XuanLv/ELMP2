#include "net/endpoint.h"

#include "base/clock.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <algorithm>
#include <format>

#pragma comment(lib, "ws2_32.lib")

namespace elmp::net {

struct Endpoint::Peer {
    std::uint32_t id;
    sockaddr_in addr;
    std::string addr_text;
    std::unique_ptr<Link> link;
    bool announced_close = false;
};

namespace {

bool wsa_up() {
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

std::string addr_text(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::format("{}:{}", ip, ntohs(a.sin_port));
}

bool same(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port; }

SOCKET open_socket(std::uint16_t port, std::string& err) {
    if (!wsa_up()) return err = "WSAStartup failed", INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return err = std::format("socket() failed ({})", WSAGetLastError()), INVALID_SOCKET;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        err = std::format("bind to UDP port {} failed ({})", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    // A full world snapshot can exceed the Windows default receive buffer in one burst.
    // Losing one packet now also blocks the ordered 60Hz Lily inputs behind its retransmit.
    int receive_bytes = 1024 * 1024;
    u_long nonblocking = 1;
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_bytes), sizeof(receive_bytes)) != 0 ||
        ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
        err = std::format("UDP receive setup failed ({})", WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    BOOL no = FALSE; // ignore ICMP port-unreachable resets on UDP
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof(no), nullptr, 0, &ret, nullptr, nullptr);
    return s;
}

} // namespace

std::string to_hex(std::span<const std::uint8_t> b) {
    std::string s;
    for (auto c : b) s += std::format("{:02x}", c);
    return s;
}

std::unique_ptr<Endpoint> Endpoint::host(std::uint16_t port, std::string_view code, const Identity& id, std::size_t max_peers, std::string& err) {
    const SOCKET s = open_socket(port, err);
    if (s == INVALID_SOCKET) return nullptr;
    std::unique_ptr<Endpoint> e(new Endpoint());
    e->host_ = true;
    e->sock_ = s;
    e->port_ = port;
    e->code_ = code;
    e->id_ = id;
    e->max_peers_ = std::max<std::size_t>(1, max_peers);
    e->status_.host = true;
    e->status_.port = port;
    e->thread_ = std::thread([p = e.get()] { p->run(); });
    return e;
}

std::unique_ptr<Endpoint> Endpoint::join(std::string_view addr, std::uint16_t port, std::string_view code, const Identity& id, std::string& err) {
    if (!wsa_up()) return err = "WSAStartup failed", nullptr;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(std::string(addr).c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return err = std::format("cannot resolve {}", addr), nullptr;
    sockaddr_in target = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    const SOCKET s = open_socket(0, err);
    if (s == INVALID_SOCKET) return nullptr;
    std::unique_ptr<Endpoint> e(new Endpoint());
    e->sock_ = s;
    e->code_ = code;
    e->id_ = id;
    auto p = std::make_unique<Peer>();
    p->id = e->next_id_++;
    p->addr = target;
    p->addr_text = addr_text(target);
    p->link = std::make_unique<Link>(Link::Role::Client, id, code, clock::now_s());
    e->peers_.push_back(std::move(p));
    e->thread_ = std::thread([q = e.get()] { q->run(); });
    return e;
}

Endpoint::~Endpoint() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (sock_ != ~std::uintptr_t{0}) closesocket(static_cast<SOCKET>(sock_));
}

void Endpoint::send_unreliable(std::uint32_t peer, std::uint8_t stream, std::span<const std::uint8_t> msg) {
    std::lock_guard lk(mu_);
    if (!fatal_.empty()) return;
    // latest-only: replace a queued message of the same peer/stream
    for (auto& c : cmds_)
        if (c.kind == Cmd::Kind::Unrel && c.peer == peer && c.stream == stream) {
            c.data.assign(msg.begin(), msg.end());
            return;
        }
    if (cmds_.size() >= kMaxCommands) return void(++status_.send_drops);
    cmds_.push_back({Cmd::Kind::Unrel, peer, stream, Bytes(msg.begin(), msg.end())});
}

void Endpoint::send_reliable(std::uint32_t peer, std::span<const std::uint8_t> msg) {
    std::lock_guard lk(mu_);
    if (!fatal_.empty()) return;
    if (cmds_.size() >= kMaxCommands) {
        ++status_.send_drops;
        return overload_locked("reliable command queue overloaded");
    }
    cmds_.push_back({Cmd::Kind::Rel, peer, 0, Bytes(msg.begin(), msg.end())});
}

void Endpoint::kick(std::uint32_t peer, std::string_view reason) {
    std::lock_guard lk(mu_);
    if (!fatal_.empty()) return;
    if (cmds_.size() >= kMaxCommands) {
        ++status_.send_drops;
        return overload_locked("control command queue overloaded");
    }
    cmds_.push_back({Cmd::Kind::Kick, peer, 0, Bytes(reason.begin(), reason.end())});
}

bool Endpoint::poll(Inbound& out) {
    std::lock_guard lk(mu_);
    if (!fatal_.empty()) {
        if (fatal_reported_) return false;
        fatal_reported_ = true;
        out = {LinkEvent::Kind::Closed, 0, 0, Reject::None, {}, fatal_};
        return true;
    }
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.erase(inbox_.begin());
    return true;
}

EndpointStatus Endpoint::status() const {
    std::lock_guard lk(mu_);
    return status_;
}

void Endpoint::overload_locked(std::string_view reason) {
    if (!fatal_.empty()) return;
    fatal_ = std::string("endpoint closed: ") + std::string(reason);
    cmds_.clear();
    inbox_.clear();
}

void Endpoint::push_in(Inbound in) {
    std::lock_guard lk(mu_);
    if (!fatal_.empty()) return;
    const bool terminal = in.kind == LinkEvent::Kind::Closed || in.kind == LinkEvent::Kind::Rejected;
    // A terminal event invalidates queued gameplay/Connected for this peer, including during join.
    if (terminal) std::erase_if(inbox_, [&](const Inbound& x) { return x.peer == in.peer; });
    if (inbox_.size() >= kMaxInbox) {
        auto it = std::find_if(inbox_.begin(), inbox_.end(), [](const Inbound& x) { return x.kind == LinkEvent::Kind::Unreliable; });
        ++status_.in_drops;
        if (it == inbox_.end()) {
            if (in.kind == LinkEvent::Kind::Unreliable) return;
            return overload_locked("reliable/control inbound queue overloaded");
        }
        inbox_.erase(it);
    }
    if (terminal) inbox_.insert(inbox_.begin(), std::move(in));
    else inbox_.push_back(std::move(in));
}

void Endpoint::flush(Peer& p) {
    for (auto& d : p.link->take_outbox())
        sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0, reinterpret_cast<const sockaddr*>(&p.addr), sizeof(p.addr));
    for (auto& e : p.link->take_events()) {
        if (e.kind == LinkEvent::Kind::Closed || e.kind == LinkEvent::Kind::Rejected) p.announced_close = true;
        push_in({e.kind, p.id, e.stream, e.reason, std::move(e.data), std::move(e.text)});
    }
}

void Endpoint::run() {
    const SOCKET s = static_cast<SOCKET>(sock_);
    std::array<std::uint8_t, 2048> buf;
    double next_status = 0;
    while (true) {
        double now = clock::now_s();
        std::string fatal;
        {
            std::lock_guard lk(mu_);
            fatal = fatal_;
        }
        if (stop_ || !fatal.empty()) {
            for (auto& p : peers_) {
                p->link->close(fatal.empty() ? "left the session" : fatal);
                flush(*p);
            }
            break;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        timeval timeout{0, 2000};
        select(0, &readable, nullptr, nullptr, &timeout);
        now = clock::now_s();
        // Drain a burst before packing ACKs. Bound work so sends/stop cannot be starved by a flood.
        for (int packet = 0; packet < 256; ++packet) {
            sockaddr_in from{};
            int flen = sizeof(from);
            const int n = recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &flen);
            if (n == SOCKET_ERROR) break;
            if (n > 0) {
                const std::span<const std::uint8_t> pkt(buf.data(), static_cast<std::size_t>(n));
                auto it = std::find_if(peers_.begin(), peers_.end(), [&](const auto& p) { return same(p->addr, from); });
                if (it != peers_.end() && (*it)->link->closed() && host_ && Link::is_hello(pkt)) { // reconnect from the same address
                    peers_.erase(it);
                    it = peers_.end();
                }
                if (it != peers_.end()) {
                    (*it)->link->on_datagram(pkt, now);
                } else if (host_ && Link::is_hello(pkt)) {
                    const auto live = std::count_if(peers_.begin(), peers_.end(), [](const auto& p) { return !p->link->closed(); });
                    if (static_cast<std::size_t>(live) >= max_peers_) {
                        const auto r = Link::reject_packet(Reject::Full);
                        sendto(s, reinterpret_cast<const char*>(r.data()), static_cast<int>(r.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
                        std::lock_guard lk(mu_);
                        ++status_.full_rejects;
                    } else {
                        auto p = std::make_unique<Peer>();
                        p->id = next_id_++;
                        p->addr = from;
                        p->addr_text = addr_text(from);
                        p->link = std::make_unique<Link>(Link::Role::Host, id_, code_, now);
                        p->link->on_datagram(pkt, now);
                        peers_.push_back(std::move(p));
                    }
                } else {
                    std::lock_guard lk(mu_);
                    ++status_.bad_source;
                }
            }
        }
        std::vector<Cmd> cmds;
        {
            std::lock_guard lk(mu_);
            if (!fatal_.empty()) continue;
            cmds.swap(cmds_);
        }
        bool overloaded = false;
        for (auto& c : cmds) {
            {
                std::lock_guard lk(mu_);
                overloaded = !fatal_.empty();
            }
            if (overloaded) break;
            for (auto& p : peers_) {
                if (c.peer && c.peer != p->id) continue;
                if (!p->link->established() && c.kind != Cmd::Kind::Kick) continue;
                if (c.kind == Cmd::Kind::Unrel) p->link->send_unreliable(c.stream, c.data);
                else if (c.kind == Cmd::Kind::Rel) {
                    if (!p->link->send_reliable(c.data)) {
                        std::lock_guard lk(mu_);
                        ++status_.send_drops;
                        overload_locked("link refused reliable message");
                        overloaded = true;
                        break;
                    }
                } else p->link->close(std::string_view(reinterpret_cast<const char*>(c.data.data()), c.data.size()));
            }
            if (overloaded) break;
        }
        if (overloaded) continue; // close every Link before polling/sending any more gameplay
        for (auto& p : peers_) {
            p->link->poll(now);
            flush(*p);
        }
        // closed links are kept briefly for status, then dropped (host); the client keeps its one link
        if (host_) std::erase_if(peers_, [](const auto& p) { return p->link->closed() && p->announced_close; });
        if (now >= next_status) {
            next_status = now + 0.25;
            std::lock_guard lk(mu_);
            status_.peers.clear();
            for (auto& p : peers_) status_.peers.push_back({p->id, p->addr_text, p->link->phase(), p->link->stats()});
        }
    }
}

} // namespace elmp::net
