#pragma once

#include "net/socket.h"
#include "seeder/dns.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <span>
#include <vector>

namespace bound {

// Milestone 8: the seeder's DNS server. Answers queries on a UDP socket from the zone and a list
// of addresses (the crawler's good nodes), a random sample in each reply, so every node gets a
// share of the newcomers.
//
// Every reply is bigger than its query, and a UDP sender's address can be forged, so a server like
// this can be used to flood a victim with replies it never asked for. Two limits keep it a poor
// tool for that: each address may ask 20 times in a burst and 10 times a second after, and
// replies stay within 512 bytes (1,232 for clients that say they take more).
class DnsServer final : public PollClient {
public:
    using Addresses = std::function<std::vector<NetAddress>()>;

    // `addresses` is asked at most every 10 seconds. The socket must be bound already.
    DnsServer(UdpSocket socket, DnsZone zone, Addresses addresses, std::uint64_t seed);

    std::uint16_t port() const { return socket_.local_port(); }
    void add_poll_items(std::vector<PollItem>& items) override;
    void handle_poll(std::span<const PollItem> items, std::int64_t now) override;

    struct Stats {
        std::uint64_t queries = 0;  // well-formed queries answered
        std::uint64_t refused = 0;  // for names outside the zone
        std::uint64_t dropped = 0;  // over the rate limit, or unparseable
    };
    const Stats& stats() const { return stats_; }

private:
    bool allow(const NetAddress& from, std::int64_t now);

    UdpSocket socket_;
    DnsZone zone_;
    Addresses addresses_;
    std::mt19937_64 rng_;
    std::vector<NetAddress> cached_;
    std::int64_t cached_at_ = -1'000'000;
    struct Bucket {
        double tokens = 0;
        std::int64_t refilled = 0;
    };
    std::map<std::array<std::uint8_t, 16>, Bucket> buckets_;  // by IP
    Stats stats_;
};

}  // namespace bound
