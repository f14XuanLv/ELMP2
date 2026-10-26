#include "coop/session.h"

#include "engine/frame.h"
#include "engine/lifecycle.h"

#include "base/log.h"
#include "base/metrics.h"
#include "base/preflight.h"

#include <elmp/build_info.h>

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <vector>

namespace elmp::coop {

namespace {

std::unique_ptr<net::Endpoint> g_ep;
Role g_role = Role::None;
Handlers g_handlers[3];
std::string g_error;
net::Identity g_identity;
bool g_identity_ready = false;
metrics::Counter& c_in = metrics::counter("coop.inbound");

std::filesystem::path module_path() {
    HMODULE m{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&module_path), &m);
    wchar_t buf[MAX_PATH * 2]{};
    const auto n = GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf)));
    return n ? std::filesystem::path(buf) : std::filesystem::path{};
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

void take16(std::array<std::uint8_t, 16>& out, const net::crypto::Digest& d) { std::copy_n(d.begin(), 16, out.begin()); }

const Handlers& h() { return g_handlers[static_cast<int>(g_role)]; }

void stop_role() {
    if (g_role != Role::None && h().stop) h().stop();
    g_ep.reset();
    g_role = Role::None;
}

void tick(const frame::Tick& t) {
    if (!g_ep) return;
    net::Inbound in;
    int budget = 512;
    while (budget-- > 0 && g_ep && g_ep->poll(in)) {
        ++c_in;
        const auto& hh = h();
        switch (in.kind) {
        case net::LinkEvent::Kind::Connected:
            log::info("coop: peer {} connected", in.peer);
            if (hh.connected) hh.connected(in.peer);
            break;
        case net::LinkEvent::Kind::Rejected:
        case net::LinkEvent::Kind::Closed:
            g_error = in.text;
            log::info("coop: peer {} {}: {}", in.peer, in.kind == net::LinkEvent::Kind::Rejected ? "rejected" : "closed", in.text);
            if (hh.closed) hh.closed(in.peer, in.text, in.reason);
            if (g_role == Role::Client) { // the client's only link is gone: the session ends
                stop_role();
                return;
            }
            break;
        case net::LinkEvent::Kind::Reliable:
            if (hh.reliable) hh.reliable(in.peer, in.data);
            break;
        case net::LinkEvent::Kind::Unreliable:
            if (hh.unreliable) hh.unreliable(in.peer, in.stream, in.data);
            break;
        }
    }
    if (g_ep && h().tick) h().tick(t.now_us);
}

} // namespace

void set_handlers(Role role, const Handlers& hh) { g_handlers[static_cast<int>(role)] = hh; }

net::Identity identity() {
    if (g_identity_ready) return g_identity;
    net::Identity id;
    id.proto = static_cast<std::uint8_t>(build::protocol);
    const auto& sha = preflight::current().sha256;
    for (std::size_t i = 0; i < 16 && 2 * i + 1 < sha.size(); ++i) id.exe[i] = static_cast<std::uint8_t>(std::stoul(sha.substr(2 * i, 2), nullptr, 16));
    const auto dll = module_path();
    const auto dll_bytes = read_file(dll);
    take16(id.dll, net::crypto::sha256(dll_bytes));
    // data content: relative path + bytes of every file under Mods/ELMP/data, sorted by path
    const auto data = dll.parent_path().parent_path() / "data";
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(data, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    std::vector<std::uint8_t> all;
    for (const auto& f : files) {
        const auto rel = std::filesystem::relative(f, data).generic_string();
        all.insert(all.end(), rel.begin(), rel.end());
        const auto d = net::crypto::sha256(read_file(f));
        all.insert(all.end(), d.begin(), d.end());
    }
    take16(id.content, net::crypto::sha256(all));
    g_identity = id;
    g_identity_ready = true;
    log::info("coop identity: proto {} exe {} dll {} data {} ({} files)", id.proto, net::to_hex(id.exe).substr(0, 8), net::to_hex(id.dll).substr(0, 8),
              net::to_hex(id.content).substr(0, 8), files.size());
    return g_identity;
}

std::string host(std::uint16_t port, std::string_view code, std::size_t max_peers) {
    if (g_ep) return "a session is already running (elmp lan leave first)";
    if (code.empty()) return "pairing code required";
    std::string err;
    g_ep = net::Endpoint::host(port, code, identity(), max_peers, err);
    if (!g_ep) return err;
    g_role = Role::Host;
    g_error.clear();
    log::info("coop: hosting on UDP {} (max {} guests)", port, max_peers);
    return {};
}

std::string join(std::string_view addr, std::uint16_t port, std::string_view code) {
    if (g_ep) return "a session is already running (elmp lan leave first)";
    if (code.empty()) return "pairing code required";
    std::string err;
    g_ep = net::Endpoint::join(addr, port, code, identity(), err);
    if (!g_ep) return err;
    g_role = Role::Client;
    g_error.clear();
    log::info("coop: joining {}:{}", addr, port);
    return {};
}

void leave(std::string_view reason) {
    if (!g_ep) return;
    log::info("coop: leaving ({})", reason);
    stop_role(); // Endpoint destructor sends F_CLOSE to every peer
}

Role role() { return g_role; }
bool active() { return g_ep != nullptr; }
void send_reliable(std::uint32_t peer, std::span<const std::uint8_t> msg) {
    if (g_ep) g_ep->send_reliable(peer, msg);
}
void send_unreliable(std::uint32_t peer, std::uint8_t stream, std::span<const std::uint8_t> msg) {
    if (g_ep) g_ep->send_unreliable(peer, stream, msg);
}
void kick(std::uint32_t peer, std::string_view reason) {
    if (g_ep) g_ep->kick(peer, reason);
}
net::EndpointStatus status() { return g_ep ? g_ep->status() : net::EndpointStatus{}; }
const std::string& last_error() { return g_error; }

std::string describe() {
    if (!g_ep) return std::format("session: none{}\n", g_error.empty() ? "" : " (last: " + g_error + ")");
    const auto s = g_ep->status();
    static const char* phases[] = {"hello", "authing", "connected", "closed"};
    std::string out = std::format("session: {} port={} peers={} fullRejects={} inDrops={} sendDrops={} badSource={}\n", g_role == Role::Host ? "host" : "client", s.port,
                                  s.peers.size(), s.full_rejects, s.in_drops, s.send_drops, s.bad_source);
    for (const auto& p : s.peers)
        out += std::format("  peer {} {} {} rtt={:.1f}ms tx={} rx={} retx={} dup={} bad={} unacked={} pending={} evDrops={}\n", p.id, p.addr, phases[static_cast<int>(p.phase)],
                           p.stats.rtt_ms, p.stats.tx_packets, p.stats.rx_packets, p.stats.retransmits, p.stats.dup, p.stats.bad, p.stats.unacked, p.stats.pending_msgs,
                           p.stats.event_drops);
    return out;
}

void install() { frame::add_phase("coop", tick, [] { return g_ep != nullptr; }); }

} // namespace elmp::coop
