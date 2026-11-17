#include "net/socket.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <stdexcept>
#include <thread>
#include <tuple>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace bound {

namespace {

#ifdef _WIN32
int last_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
bool connect_in_progress(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
void close_handle(SocketHandle h) { closesocket(h); }
bool set_nonblocking(SocketHandle h) {
    u_long on = 1;
    return ioctlsocket(h, FIONBIO, &on) == 0;
}
using IoLength = int;
constexpr int kSendFlags = 0;
#else
int last_error() { return errno; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
bool connect_in_progress(int error) { return error == EINPROGRESS; }
void close_handle(SocketHandle h) { ::close(h); }
bool set_nonblocking(SocketHandle h) {
    const int flags = fcntl(h, F_GETFL, 0);
    return flags != -1 && fcntl(h, F_SETFL, flags | O_NONBLOCK) == 0;
}
using IoLength = std::size_t;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // a closed peer must not kill us with SIGPIPE
#else
constexpr int kSendFlags = 0;
#endif
#endif

void set_option(SocketHandle h, int level, int name, int value) {
    setsockopt(h, level, name, reinterpret_cast<const char*>(&value), sizeof value);
}

// Lets a restarted node listen on its port again at once, but (on Windows) without letting
// another program take over a port that's in use.
void set_reuse(SocketHandle h) {
#ifdef _WIN32
    set_option(h, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    set_option(h, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
}

// Small messages (pings, headers) go out at once instead of waiting to be batched.
void set_nodelay(SocketHandle h) { set_option(h, IPPROTO_TCP, TCP_NODELAY, 1); }

std::runtime_error socket_error(const std::string& what) {
    return std::runtime_error(std::format("{} (socket error {})", what, last_error()));
}

std::pair<sockaddr_storage, socklen_t> to_sockaddr(const NetAddress& a) {
    sockaddr_storage storage{};
    if (a.is_ipv4()) {
        auto* in = reinterpret_cast<sockaddr_in*>(&storage);
        in->sin_family = AF_INET;
        in->sin_port = htons(a.port);
        std::memcpy(&in->sin_addr, a.ip.data() + 12, 4);
        return {storage, static_cast<socklen_t>(sizeof(sockaddr_in))};
    }
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&storage);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(a.port);
    std::memcpy(&in6->sin6_addr, a.ip.data(), 16);
    return {storage, static_cast<socklen_t>(sizeof(sockaddr_in6))};
}

NetAddress from_sockaddr(const sockaddr* sa) {
    NetAddress a;
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        a.ip[10] = a.ip[11] = 0xff;
        std::memcpy(a.ip.data() + 12, &in->sin_addr, 4);
        a.port = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        std::memcpy(a.ip.data(), &in6->sin6_addr, 16);
        a.port = ntohs(in6->sin6_port);
    }
    return a;
}

}  // namespace

NetAddress NetAddress::ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    NetAddress addr;
    addr.ip = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, a, b, c, d};
    addr.port = port;
    return addr;
}

bool NetAddress::is_ipv4() const {
    static constexpr std::array<std::uint8_t, 12> kPrefix{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return std::equal(kPrefix.begin(), kPrefix.end(), ip.begin());
}

bool NetAddress::is_unspecified() const {
    return std::all_of(ip.begin(), ip.end(), [](std::uint8_t b) { return b == 0; }) ||
           (is_ipv4() && ip[12] == 0 && ip[13] == 0 && ip[14] == 0 && ip[15] == 0);
}

bool NetAddress::is_loopback() const {
    if (is_ipv4()) return ip[12] == 127;
    static constexpr std::array<std::uint8_t, 16> kLoopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return ip == kLoopback;
}

bool NetAddress::is_routable() const {
    if (is_unspecified() || is_loopback()) return false;
    if (is_ipv4()) {
        const std::uint8_t a = ip[12];
        const std::uint8_t b = ip[13];
        const std::uint8_t c = ip[14];
        if (a == 0 || a == 10 || a >= 224) return false;            // "this network", RFC 1918, multicast and reserved
        if (a == 172 && b >= 16 && b <= 31) return false;            // RFC 1918
        if (a == 192 && b == 168) return false;                      // RFC 1918
        if (a == 169 && b == 254) return false;                      // link-local (RFC 3927)
        if (a == 100 && b >= 64 && b <= 127) return false;           // carrier-grade NAT (RFC 6598)
        if (a == 198 && (b == 18 || b == 19)) return false;          // benchmarking (RFC 2544)
        if (a == 192 && b == 0 && c == 2) return false;              // documentation (RFC 5737)
        if (a == 198 && b == 51 && c == 100) return false;
        if (a == 203 && b == 0 && c == 113) return false;
        return true;
    }
    if (ip[0] == 0xff) return false;                                  // multicast
    if ((ip[0] & 0xfe) == 0xfc) return false;                         // unique local (fc00::/7)
    if (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80) return false;        // link-local (fe80::/10)
    if (ip[0] == 0x20 && ip[1] == 0x01 && ip[2] == 0x0d && ip[3] == 0xb8) return false;  // documentation (2001:db8::/32)
    return true;
}

std::string NetAddress::to_string() const {
    return is_ipv4() ? std::format("{}:{}", ip_string(), port) : std::format("[{}]:{}", ip_string(), port);
}

std::string NetAddress::ip_string() const {
    if (is_ipv4()) return std::format("{}.{}.{}.{}", ip[12], ip[13], ip[14], ip[15]);
    init_sockets();
    char text[INET6_ADDRSTRLEN] = {};
    in6_addr raw{};
    std::memcpy(&raw, ip.data(), 16);
    inet_ntop(AF_INET6, &raw, text, sizeof text);
    return text;
}

std::optional<std::pair<std::string, std::uint16_t>> split_host_port(std::string_view text, std::uint16_t default_port) {
    std::string_view host = text;
    std::string_view port;
    if (!text.empty() && text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host = text.substr(1, close - 1);
        const std::string_view rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return std::nullopt;
            port = rest.substr(1);
            if (port.empty()) return std::nullopt;
        }
    } else if (std::count(text.begin(), text.end(), ':') == 1) {
        const std::size_t colon = text.find(':');
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
        if (port.empty()) return std::nullopt;
    }  // with more colons and no brackets, it's a bare IPv6 address
    if (host.empty()) return std::nullopt;

    std::uint16_t number = default_port;
    if (!port.empty()) {
        unsigned value = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (error != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535) return std::nullopt;
        number = static_cast<std::uint16_t>(value);
    }
    return std::pair{std::string(host), number};
}

std::vector<NetAddress> resolve(std::string_view text, std::uint16_t default_port) {
    init_sockets();
    const auto split = split_host_port(text, default_port);
    if (!split) return {};
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(split->first.c_str(), nullptr, &hints, &list) != 0) return {};
    std::vector<NetAddress> found;
    for (const addrinfo* ai = list; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        NetAddress a = from_sockaddr(ai->ai_addr);
        a.port = split->second;
        if (std::find(found.begin(), found.end(), a) == found.end()) found.push_back(a);
    }
    freeaddrinfo(list);
    return found;
}

void init_sockets() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("can't start Winsock");
        return true;
    }();
    (void)started;
#endif
}

Socket::~Socket() {
    if (valid()) close_handle(handle_);
}

Socket::Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, kInvalidSocket)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (valid()) close_handle(handle_);
        handle_ = std::exchange(other.handle_, kInvalidSocket);
    }
    return *this;
}

Socket Socket::listen(std::uint16_t port, bool loopback_only) {
    init_sockets();
    if (!loopback_only) {
        // One IPv6 socket that takes IPv4 connections too (IPV6_V6ONLY off).
        Socket s(::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP));
        if (s.valid()) {
            set_option(s.handle_, IPPROTO_IPV6, IPV6_V6ONLY, 0);
            set_reuse(s.handle_);
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(port);
            addr.sin6_addr = in6addr_any;
            if (bind(s.handle_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0 &&
                ::listen(s.handle_, SOMAXCONN) == 0 && set_nonblocking(s.handle_)) {
                return s;
            }
        }
    }
    // IPv4 only: when IPv6 isn't available, or for loopback.
    Socket s(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) throw socket_error("can't create a socket");
    set_reuse(s.handle_);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(s.handle_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        throw socket_error(std::format("can't listen on port {}; is it in use?", port));
    }
    if (::listen(s.handle_, SOMAXCONN) != 0 || !set_nonblocking(s.handle_)) throw socket_error("can't listen");
    return s;
}

Socket Socket::connect(const NetAddress& to) {
    init_sockets();
    const auto [addr, length] = to_sockaddr(to);
    Socket s(::socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP));
    if (!s.valid()) throw socket_error("can't create a socket");
    if (!set_nonblocking(s.handle_)) throw socket_error("can't make a socket non-blocking");
    set_nodelay(s.handle_);
    if (::connect(s.handle_, reinterpret_cast<const sockaddr*>(&addr), length) != 0 && !connect_in_progress(last_error())) {
        throw socket_error("can't connect to " + to.to_string());
    }
    return s;
}

std::uint16_t Socket::local_port() const {
    sockaddr_storage addr{};
    socklen_t length = sizeof addr;
    if (getsockname(handle_, reinterpret_cast<sockaddr*>(&addr), &length) != 0) return 0;
    return from_sockaddr(reinterpret_cast<const sockaddr*>(&addr)).port;
}

int Socket::connect_error() const {
    int error = 0;
    socklen_t length = sizeof error;
    if (getsockopt(handle_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0) return last_error();
    return error;
}

std::optional<std::pair<Socket, NetAddress>> Socket::accept() const {
    sockaddr_storage addr{};
    socklen_t length = sizeof addr;
    Socket s(::accept(handle_, reinterpret_cast<sockaddr*>(&addr), &length));
    if (!s.valid() || !set_nonblocking(s.handle_)) return std::nullopt;  // none waiting, or it already went away
    set_nodelay(s.handle_);
    return std::pair{std::move(s), from_sockaddr(reinterpret_cast<const sockaddr*>(&addr))};
}

IoResult Socket::send(ByteSpan data) const {
    const auto length = static_cast<IoLength>(std::min<std::size_t>(data.size(), 1 << 20));
    const auto sent = ::send(handle_, reinterpret_cast<const char*>(data.data()), length, kSendFlags);
    if (sent >= 0) return {static_cast<std::size_t>(sent), false, false};
    if (would_block(last_error())) return {0, true, false};
    return {0, false, true};
}

IoResult Socket::recv(std::span<std::uint8_t> buffer) const {
    const auto length = static_cast<IoLength>(std::min<std::size_t>(buffer.size(), 1 << 20));
    const auto got = ::recv(handle_, reinterpret_cast<char*>(buffer.data()), length, 0);
    if (got > 0) return {static_cast<std::size_t>(got), false, false};
    if (got == 0) return {0, false, true};  // orderly close
    if (would_block(last_error())) return {0, true, false};
    return {0, false, true};
}

UdpSocket::~UdpSocket() {
    if (valid()) close_handle(handle_);
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidSocket)), ipv6_(other.ipv6_) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        if (valid()) close_handle(handle_);
        handle_ = std::exchange(other.handle_, kInvalidSocket);
        ipv6_ = other.ipv6_;
    }
    return *this;
}

UdpSocket UdpSocket::bind(std::uint16_t port, const std::optional<NetAddress>& address) {
    init_sockets();
    const auto fail = [&] { return socket_error(std::format("can't bind UDP port {}; is it in use?", port)); };
    UdpSocket s;
    if (!address || !address->is_ipv4()) {
        // IPv6: every interface with IPv4 too (IPV6_V6ONLY off), or one IPv6 address.
        s.handle_ = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (s.valid()) {
            set_option(s.handle_, IPPROTO_IPV6, IPV6_V6ONLY, address ? 1 : 0);
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(port);
            if (address) {
                std::memcpy(&addr.sin6_addr, address->ip.data(), 16);
            } else {
                addr.sin6_addr = in6addr_any;
            }
            if (::bind(s.handle_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0 && set_nonblocking(s.handle_)) {
                s.ipv6_ = true;
                return s;
            }
            if (address) throw fail();
            close_handle(s.handle_);  // no IPv6 here: fall back to IPv4
            s.handle_ = kInvalidSocket;
        } else if (address) {
            throw socket_error("can't create a UDP socket");
        }
    }
    s.handle_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!s.valid()) throw socket_error("can't create a UDP socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (address) {
        std::memcpy(&addr.sin_addr, address->ip.data() + 12, 4);
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (::bind(s.handle_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || !set_nonblocking(s.handle_)) throw fail();
    return s;
}

std::uint16_t UdpSocket::local_port() const {
    sockaddr_storage addr{};
    socklen_t length = sizeof addr;
    if (getsockname(handle_, reinterpret_cast<sockaddr*>(&addr), &length) != 0) return 0;
    return from_sockaddr(reinterpret_cast<const sockaddr*>(&addr)).port;
}

std::optional<std::pair<std::size_t, NetAddress>> UdpSocket::receive(std::span<std::uint8_t> buffer) const {
    // A few tries: on Windows a send to a closed port comes back as an error on a later receive
    // (WSAECONNRESET), and a datagram longer than the buffer as WSAEMSGSIZE. Neither means there's
    // nothing more waiting.
    for (int tries = 0; tries < 8; ++tries) {
        sockaddr_storage from{};
        socklen_t length = sizeof from;
        const auto got = ::recvfrom(handle_, reinterpret_cast<char*>(buffer.data()), static_cast<IoLength>(buffer.size()), 0,
                                    reinterpret_cast<sockaddr*>(&from), &length);
        if (got >= 0) return std::pair{static_cast<std::size_t>(got), from_sockaddr(reinterpret_cast<const sockaddr*>(&from))};
        if (would_block(last_error())) return std::nullopt;
    }
    return std::nullopt;
}

bool UdpSocket::send(ByteSpan data, const NetAddress& to) const {
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (ipv6_) {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&storage);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(to.port);
        std::memcpy(&in6->sin6_addr, to.ip.data(), 16);  // an IPv4 address is already in its mapped form
        length = sizeof(sockaddr_in6);
    } else {
        if (!to.is_ipv4()) return false;
        std::tie(storage, length) = to_sockaddr(to);
    }
    const auto sent = ::sendto(handle_, reinterpret_cast<const char*>(data.data()), static_cast<IoLength>(data.size()), kSendFlags,
                               reinterpret_cast<const sockaddr*>(&storage), length);
    return sent >= 0 && static_cast<std::size_t>(sent) == data.size();
}

void poll(std::span<PollItem> items, int timeout_ms) {
    if (items.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return;
    }
    std::vector<pollfd> fds(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        fds[i].fd = items[i].handle;
        fds[i].events = static_cast<short>((items[i].want_read ? POLLRDNORM : 0) | (items[i].want_write ? POLLWRNORM : 0));
    }
#ifdef _WIN32
    WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
#else
    ::poll(fds.data(), fds.size(), timeout_ms);
#endif
    for (std::size_t i = 0; i < items.size(); ++i) {
        const short revents = fds[i].revents;
        items[i].readable = (revents & POLLRDNORM) != 0;
        items[i].writable = (revents & POLLWRNORM) != 0;
        items[i].failed = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
    }
}

}  // namespace bound
