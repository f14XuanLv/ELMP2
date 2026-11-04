#pragma once

// Process side of the transport: one UDP socket, one network thread, a peer table of Links (C14,
// F2.11: multi-peer from the start). Host: every guest address gets its own Link (own handshake, keys,
// reliable state); more than `max_peers` → plaintext Reject::Full to that address. Client: one Link to
// the host, datagrams from other addresses ignored. Peer ids are monotonic from 1 and never reused.
// The game thread talks to the network thread through two bounded mutex queues. Reliable/control
// overload latches a queue-independent, one-shot peer=0 Closed and terminates the whole endpoint.

#include "net/transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace elmp::net {

struct Inbound {
    LinkEvent::Kind kind;
    std::uint32_t peer = 0;
    std::uint8_t stream = 0;
    Reject reason = Reject::None;
    Bytes data;
    std::string text;
};

struct PeerStatus {
    std::uint32_t id = 0;
    std::string addr;
    Link::Phase phase = Link::Phase::Hello;
    LinkStats stats;
};

struct EndpointStatus {
    bool host = false;
    std::uint16_t port = 0;
    std::vector<PeerStatus> peers;
    std::uint64_t full_rejects = 0, in_drops = 0, send_drops = 0, bad_source = 0;
};

class Endpoint {
public:
    static constexpr std::size_t kMaxInbox = 512;
    static constexpr std::size_t kMaxCommands = 1024;

    // Throw-free factories: nullptr + err on failure (socket / bind / resolve).
    static std::unique_ptr<Endpoint> host(std::uint16_t port, std::string_view code, const Identity& id, std::size_t max_peers, std::string& err);
    static std::unique_ptr<Endpoint> join(std::string_view addr, std::uint16_t port, std::string_view code, const Identity& id, std::string& err);
    ~Endpoint(); // closes every link (F_CLOSE) and joins the thread

    void send_unreliable(std::uint32_t peer, std::uint8_t stream, std::span<const std::uint8_t> msg); // peer 0 = every established peer
    void send_reliable(std::uint32_t peer, std::span<const std::uint8_t> msg);
    void kick(std::uint32_t peer, std::string_view reason);
    bool poll(Inbound& out); // game thread
    EndpointStatus status() const;
    bool is_host() const { return host_; }

private:
    struct Peer;
    struct Cmd {
        enum class Kind : std::uint8_t { Unrel, Rel, Kick } kind;
        std::uint32_t peer;
        std::uint8_t stream;
        Bytes data;
    };
    Endpoint() = default;
    void run();
    void push_in(Inbound in);
    void flush(Peer& p);
    void overload_locked(std::string_view reason); // mu_ held; permanent until endpoint destruction

    bool host_ = false;
    std::uintptr_t sock_ = ~std::uintptr_t{0};
    std::uint16_t port_ = 0;
    std::string code_;
    Identity id_{};
    std::size_t max_peers_ = 1;
    std::vector<std::unique_ptr<Peer>> peers_; // network thread only
    std::uint32_t next_id_ = 1;

    mutable std::mutex mu_; // guards cmds_, inbox_, status_, fatal_ and fatal_reported_
    std::vector<Cmd> cmds_;
    std::vector<Inbound> inbox_;
    EndpointStatus status_;
    std::string fatal_;
    bool fatal_reported_ = false;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::string to_hex(std::span<const std::uint8_t> b);

} // namespace elmp::net
