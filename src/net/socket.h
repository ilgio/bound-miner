#pragma once

#include "util/bytes.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bound {

// Milestone 6: raw OS sockets (Winsock on Windows, BSD sockets elsewhere), the way Bitcoin Core's
// CConnman uses them: non-blocking TCP sockets and one poll loop.

// A peer's address: 16 bytes of IPv6, with IPv4 mapped into ::ffff:0:0/96 as Bitcoin does, plus a
// TCP port.
struct NetAddress {
    std::array<std::uint8_t, 16> ip{};
    std::uint16_t port = 0;

    static NetAddress ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port);
    bool is_ipv4() const;
    bool is_unspecified() const;  // all-zero IP: never a usable address
    bool is_loopback() const;     // 127.0.0.0/8 or ::1: this machine
    // Milestone 8: an address on the public internet. Not loopback, private (10/8, 172.16/12,
    // 192.168/16, fc00::/7), link-local, carrier NAT (100.64/10), documentation, multicast or
    // reserved space. A node on a public network neither dials nor passes on the others: an addr
    // message naming 192.168.1.1 would otherwise have every node probe its own LAN.
    bool is_routable() const;
    std::string to_string() const;  // "1.2.3.4:18333" or "[2001:db8::1]:18333"
    std::string ip_string() const;  // "1.2.3.4" or "2001:db8::1"

    friend auto operator<=>(const NetAddress&, const NetAddress&) = default;
};

// "host:port", "[ipv6]:port", or a bare host or IPv6 address with default_port. nullopt if the
// host is empty or the port isn't a number from 1 to 65535.
std::optional<std::pair<std::string, std::uint16_t>> split_host_port(std::string_view text, std::uint16_t default_port);
// Looks the host up: a literal address, or a name through DNS. Empty if it can't be resolved.
std::vector<NetAddress> resolve(std::string_view text, std::uint16_t default_port);

#ifdef _WIN32
using SocketHandle = std::uintptr_t;  // Winsock's SOCKET
inline constexpr SocketHandle kInvalidSocket = ~SocketHandle{0};
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// Starts the OS socket library once per process (WSAStartup on Windows). Everything below calls it.
void init_sockets();

// How a non-blocking send or receive went.
struct IoResult {
    std::size_t bytes = 0;
    bool would_block = false;  // nothing could move right now; poll says when to try again
    bool closed = false;       // the connection is gone: closed by the peer, or an error
};

// An owned, non-blocking TCP socket; closed when destroyed.
class Socket {
public:
    Socket() = default;
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Listens on every interface, IPv6 and IPv4 (or only on 127.0.0.1). Port 0 lets the OS pick a
    // free one. Throws std::runtime_error on failure, e.g. when the port is taken.
    static Socket listen(std::uint16_t port, bool loopback_only);
    // Starts connecting without waiting. The socket turns writable when the attempt ends;
    // connect_error() then says how it went. Throws std::runtime_error if it can't even start.
    static Socket connect(const NetAddress& to);

    bool valid() const { return handle_ != kInvalidSocket; }
    SocketHandle handle() const { return handle_; }
    std::uint16_t local_port() const;
    int connect_error() const;  // 0 once a connect has succeeded
    // A connection waiting on a listening socket, or nullopt.
    std::optional<std::pair<Socket, NetAddress>> accept() const;
    IoResult send(ByteSpan data) const;
    IoResult recv(std::span<std::uint8_t> buffer) const;

private:
    explicit Socket(SocketHandle handle) : handle_(handle) {}

    SocketHandle handle_ = kInvalidSocket;
};

// Milestone 8: a non-blocking UDP socket, for the DNS seeder. Datagrams aren't a stream: each
// receive() is one whole packet from one sender, and a send() either goes out whole or not at all.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Binds `port` (0: the OS picks) on every interface, IPv6 and IPv4; or, given `address`, on
    // that address alone (its port is ignored). Throws std::runtime_error if the port is taken.
    static UdpSocket bind(std::uint16_t port, const std::optional<NetAddress>& address = std::nullopt);

    bool valid() const { return handle_ != kInvalidSocket; }
    SocketHandle handle() const { return handle_; }
    std::uint16_t local_port() const;
    // The next waiting datagram: how many bytes of it went into `buffer` (the rest of a longer one
    // is lost) and who sent it. nullopt when none is waiting.
    std::optional<std::pair<std::size_t, NetAddress>> receive(std::span<std::uint8_t> buffer) const;
    // false if it couldn't go out. Nothing is queued or retried.
    bool send(ByteSpan data, const NetAddress& to) const;

private:
    SocketHandle handle_ = kInvalidSocket;
    bool ipv6_ = false;  // an IPv6 socket: IPv4 peers are addressed as ::ffff:a.b.c.d
};

struct PollItem {
    SocketHandle handle = kInvalidSocket;
    bool want_read = false;
    bool want_write = false;
    // Set by poll():
    bool readable = false;
    bool writable = false;
    bool failed = false;  // an error, or the other side hung up
};

// Waits until at least one item is ready, or timeout_ms passes.
void poll(std::span<PollItem> items, int timeout_ms);

// Milestone 7: something with sockets of its own (the RPC server) that shares another's poll loop,
// and so its thread: no locks needed between them.
class PollClient {
public:
    virtual ~PollClient() = default;
    // Appends the sockets to wait for.
    virtual void add_poll_items(std::vector<PollItem>& items) = 0;
    // The same items, after poll() filled them in; then do the work.
    virtual void handle_poll(std::span<const PollItem> items, std::int64_t now) = 0;
};

}  // namespace bound
