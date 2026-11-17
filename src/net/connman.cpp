#include "net/connman.h"

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <stdexcept>
#include <utility>

namespace bound {

namespace {

constexpr int kPollMillis = 100;
constexpr std::size_t kReadChunk = 64 << 10;
constexpr std::size_t kMaxReadPerPass = 1 << 20;  // per connection, so one busy peer can't starve the rest

}  // namespace

Connman::Connman(PeerManager& peers, AddrMan& addrman, BanMan& banman, ConnmanOptions options, PeerManager::Logger log,
                 Clock clock)
    : peers_(peers),
      addrman_(addrman),
      banman_(banman),
      options_(std::move(options)),
      log_(std::move(log)),
      clock_(std::move(clock)) {
    if (options_.listen) listener_ = Socket::listen(options_.port, options_.loopback_only);
    for (const std::string& target : options_.connect) manual_.push_back({target});
}

Connman::~Connman() {
    for (auto& [id, c] : connections_) peers_.remove_peer(id);
}

std::uint16_t Connman::port() const { return listener_.valid() ? listener_.local_port() : 0; }

void Connman::run(const std::function<void()>& each_loop) {
    while (!stopping_) {
        const std::int64_t now = clock_();
        if (started_ == 0) started_ = now;
        use_seeds(now);
        open_connections(now);

        // What to wait for: the listener, connects in progress, and every connection (for
        // writing too when there's something queued for it).
        std::vector<PollItem> items;
        if (listener_.valid()) items.push_back({listener_.handle(), true, false});
        for (const Connecting& c : connecting_) items.push_back({c.socket.handle(), false, true});
        for (const auto& [id, c] : connections_) {
            // Not read while its unprocessed messages pile up: TCP then holds the sender back.
            items.push_back({c.socket.handle(), !peers_.read_paused(id), !peers_.pending(id).empty()});
        }
        const std::size_t p2p_items = items.size();
        std::vector<std::size_t> client_start;
        for (PollClient* client : clients_) {
            client_start.push_back(items.size());
            client->add_poll_items(items);
        }
        client_start.push_back(items.size());
        poll(items, kPollMillis);

        std::size_t i = 0;
        if (listener_.valid() && items[i++].readable) accept_all(now);

        std::vector<Connecting> still_connecting;
        for (Connecting& c : connecting_) {
            const PollItem& item = items[i++];
            const bool done = item.writable || item.failed;
            if (done && !item.failed && c.socket.connect_error() == 0) {
                const PeerId id = next_id_++;
                if (c.manual) manual_[*c.manual].delay = 5;
                connections_.emplace(id, Connection{std::move(c.socket), c.addr, false, c.manual});
                peers_.add_peer(id, c.addr, false, now, c.manual.has_value());
            } else if (done || now - c.started > options_.connect_timeout) {
                log(std::format("can't connect to {}", c.addr.to_string()));
                if (c.manual) manual_[*c.manual].busy = false;
            } else {
                still_connecting.push_back(std::move(c));
            }
        }
        connecting_ = std::move(still_connecting);

        std::vector<PeerId> drop;
        for (auto& [id, c] : connections_) {
            if (i >= p2p_items) break;  // connected during this pass: not polled yet
            const PollItem& item = items[i++];
            if ((item.readable || item.failed) && !read(id, c, now)) drop.push_back(id);
        }
        for (PeerId id : drop) close(id);

        for (std::size_t k = 0; k < clients_.size(); ++k) {
            clients_[k]->handle_poll(std::span<const PollItem>(items).subspan(client_start[k], client_start[k + 1] - client_start[k]), now);
        }
        if (each_loop) each_loop();
        for (PeerId id : peers_.tick(clock_())) close(id);

        // Send what got queued during this pass now, rather than after the next poll.
        drop.clear();
        for (auto& [id, c] : connections_) {
            if (!write(id, c)) drop.push_back(id);
        }
        for (PeerId id : drop) close(id);
    }
    for (auto& [id, c] : connections_) peers_.remove_peer(id);
    connections_.clear();
    connecting_.clear();
}

void Connman::use_seeds(std::int64_t now) {
    if (options_.seeds.empty()) return;
    if (const std::optional<std::vector<NetAddress>> found = resolver_.take()) {
        // Dated three days back, as Core does: usable, but not passed on as freshly seen.
        std::size_t added = 0;
        for (const NetAddress& addr : *found) added += addrman_.add(addr, now - 3 * 24 * 60 * 60) ? 1 : 0;
        log(std::format("seeds: {} addresses, {} of them new", found->size(), added));
    }
    if (now < next_seeding_) return;
    std::size_t outbound = 0;
    for (const PeerInfo& peer : peers_.peers()) outbound += peer.ready && !peer.inbound ? 1 : 0;
    if (outbound >= 2) {
        next_seeding_ = now + 30;  // doing fine without them
        return;
    }
    if (addrman_.size() > 0 && now - started_ < 11) return;  // give the addresses we know a chance first
    if (resolver_.start(options_.seeds, options_.default_port)) {
        log("few peers: asking the seeds for addresses");
        next_seeding_ = now + 600;
    }
}

void Connman::open_connections(std::int64_t now) {
    std::set<NetAddress> busy = peers_.self_addresses();
    for (const auto& [id, c] : connections_) busy.insert(c.addr);
    for (const Connecting& c : connecting_) busy.insert(c.addr);

    // The --connect peers, each kept connected and retried with a growing delay.
    for (std::size_t m = 0; m < manual_.size(); ++m) {
        Manual& manual = manual_[m];
        if (manual.busy || now < manual.next_try) continue;
        manual.next_try = now + manual.delay;
        manual.delay = std::min<std::int64_t>(manual.delay * 2, 300);
        const std::vector<NetAddress> found = resolve(manual.target, options_.default_port);
        if (found.empty()) {
            log("can't resolve " + manual.target);
            continue;
        }
        start_connect(found.front(), m, now);
        busy.insert(found.front());
    }

    // Then addresses learned from peers, up to the outbound limit.
    std::size_t outbound = connecting_.size();
    for (const auto& [id, c] : connections_) outbound += c.inbound ? 0 : 1;
    for (int tries = 0; outbound < options_.max_outbound && tries < 100; ++tries) {
        const std::optional<NetAddress> addr = addrman_.select(busy, now);
        if (!addr) break;
        if (banman_.refuses(*addr, now)) {
            busy.insert(*addr);
            continue;
        }
        start_connect(*addr, std::nullopt, now);
        busy.insert(*addr);
        ++outbound;
    }
}

void Connman::start_connect(const NetAddress& addr, std::optional<std::size_t> manual, std::int64_t now) {
    addrman_.attempt(addr, now);
    try {
        connecting_.push_back({Socket::connect(addr), addr, now, manual});
        if (manual) manual_[*manual].busy = true;
    } catch (const std::runtime_error& e) {
        log(e.what());
    }
}

void Connman::accept_all(std::int64_t now) {
    while (std::optional<std::pair<Socket, NetAddress>> accepted = listener_.accept()) {
        const auto inbound = static_cast<std::size_t>(
            std::count_if(connections_.begin(), connections_.end(), [](const auto& c) { return c.second.inbound; }));
        if (inbound >= options_.max_inbound) continue;  // the socket closes as it goes out of scope
        if (banman_.refuses(accepted->second, now)) continue;
        const auto same_ip = static_cast<std::size_t>(std::count_if(connections_.begin(), connections_.end(), [&](const auto& c) {
            return c.second.inbound && c.second.addr.ip == accepted->second.ip;
        }));
        if (same_ip >= options_.max_inbound_per_ip && !accepted->second.is_loopback()) continue;
        const PeerId id = next_id_++;
        connections_.emplace(id, Connection{std::move(accepted->first), accepted->second, true, std::nullopt});
        peers_.add_peer(id, accepted->second, true, now);
    }
}

bool Connman::read(PeerId id, Connection& c, std::int64_t now) {
    std::array<std::uint8_t, kReadChunk> buffer;
    for (std::size_t total = 0; total < kMaxReadPerPass;) {
        const IoResult r = c.socket.recv(buffer);
        if (r.would_block) return true;
        if (r.closed) return false;
        if (!peers_.receive(id, ByteSpan(buffer.data(), r.bytes), now)) return false;
        total += r.bytes;
    }
    return true;
}

bool Connman::write(PeerId id, Connection& c) {
    while (true) {
        const ByteSpan out = peers_.pending(id);
        if (out.empty()) return true;
        const IoResult r = c.socket.send(out);
        if (r.closed) return false;
        if (r.would_block || r.bytes == 0) return true;
        peers_.sent(id, r.bytes);
    }
}

void Connman::close(PeerId id) {
    const auto found = connections_.find(id);
    if (found == connections_.end()) return;
    if (found->second.manual) manual_[*found->second.manual].busy = false;
    peers_.remove_peer(id);
    connections_.erase(found);  // closes the socket
}

void Connman::log(const std::string& text) const {
    if (log_) log_(text);
}

}  // namespace bound
