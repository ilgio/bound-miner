#pragma once

#include "net/socket.h"
#include "util/bytes.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <vector>

namespace bound {

// Milestone 6: the listening addresses a node knows of, learned from peers' addr messages and from
// the listening port each inbound peer announces (Bitcoin's AddrMan, without its bucket scheme
// against eclipse attacks). It picks addresses for outbound connections and answers getaddr.
struct AddrInfo {
    NetAddress addr;
    std::int64_t last_seen = 0;     // the latest time anyone vouched it was up
    std::int64_t last_try = 0;      // our latest connection attempt
    std::int64_t last_success = 0;  // our latest completed handshake with it
    int attempts = 0;               // failed attempts since the last success

    friend bool operator==(const AddrInfo&, const AddrInfo&) = default;
};

class AddrMan {
public:
    static constexpr std::size_t kMaxSize = 10000;

    explicit AddrMan(std::uint64_t seed);

    // Milestone 8. On: only addresses on the public internet are kept (NetAddress::is_routable),
    // and any others already here are dropped. Off (the default): everything, for tests and for
    // nodes on one machine or one LAN.
    void set_routable_only(bool on);

    // Learns an address, or moves its last_seen forward. Returns true if it's new. An unspecified
    // address or port 0 is refused. When full, an address we never reached makes room, the one seen
    // longest ago first; only if there's none, the proven one seen longest ago. So a flood of
    // made-up addresses can't push out the ones known to work.
    bool add(const NetAddress& addr, std::int64_t seen);
    void attempt(const NetAddress& addr, std::int64_t now);  // starting to connect
    void good(const NetAddress& addr, std::int64_t now);     // a handshake with it completed
    void remove(const NetAddress& addr);
    // A random address to try, not in `exclude` and not waiting out its retry delay (a minute,
    // doubling with each failed attempt, at most a day). Half the time one we've reached before,
    // if there is one (Bitcoin's "tried" table), so made-up addresses can't crowd out working
    // ones. Hopeless addresses (is_terrible) met on the way are forgotten. nullopt if there's none.
    std::optional<NetAddress> select(const std::set<NetAddress>& exclude, std::int64_t now);
    // Up to n of them at random, for a getaddr reply; hopeless ones (as of `now`) left out.
    std::vector<AddrInfo> sample(std::size_t n, std::int64_t now = 0);
    // Not worth keeping (Bitcoin's IsTerrible): not seen for 30 days, or tried 3 times without
    // ever answering, or failing 10 times in a row with the last success over a week ago.
    static bool is_terrible(const AddrInfo& info, std::int64_t now);

    const AddrInfo* find(const NetAddress& addr) const;
    std::size_t size() const { return addrs_.size(); }

    // The peers.dat format: version byte, compact_size count, then each entry.
    Bytes serialize() const;
    static AddrMan deserialize(ByteSpan data, std::uint64_t seed);  // throws DeserializeError

private:
    std::map<NetAddress, AddrInfo> addrs_;
    std::mt19937_64 rng_;
    bool routable_only_ = false;
};

}  // namespace bound
