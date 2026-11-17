#pragma once

#include "net/socket.h"
#include "util/bytes.h"

#include <array>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace bound {

// Hardening: the addresses this node won't talk to (Bitcoin Core's BanMan). Two lists, both by IP
// alone, so a peer can't come back on another port:
//  - discouraged: peers that broke a protocol rule, for 24 hours. Automatic, kept in memory only.
//  - banned: addresses the operator banned through RPC (setban), until a chosen time. Saved to
//    banlist.dat.
// Neither is ever dialled, and neither may connect to us. Loopback addresses and --connect peers
// are never discouraged (PeerManager's rule): on one machine that would cut off every local node.
class BanMan {
public:
    using Ip = std::array<std::uint8_t, 16>;
    static constexpr std::int64_t kDiscourageTime = 24 * 60 * 60;
    static constexpr std::size_t kMaxDiscouraged = 50'000;  // then the one expiring soonest goes

    void discourage(const NetAddress& addr, std::int64_t now);
    bool is_discouraged(const NetAddress& addr, std::int64_t now) const;

    void ban(const NetAddress& addr, std::int64_t until);
    bool unban(const NetAddress& addr);  // false if it wasn't banned
    bool is_banned(const NetAddress& addr, std::int64_t now) const;
    void clear_banned() { banned_.clear(); }
    // Current bans, by IP (the port is 0), with when each ends. Expired ones are dropped on the way.
    std::vector<std::pair<NetAddress, std::int64_t>> banned(std::int64_t now);

    bool refuses(const NetAddress& addr, std::int64_t now) const { return is_banned(addr, now) || is_discouraged(addr, now); }

    // The banlist.dat format: version byte, compact_size count, then (16-byte IP, i64 end) each.
    // Only the bans: discouragement doesn't survive a restart.
    Bytes serialize() const;
    static BanMan deserialize(ByteSpan data);  // throws DeserializeError

private:
    std::map<Ip, std::int64_t> discouraged_;  // IP -> when it ends
    std::map<Ip, std::int64_t> banned_;
};

}  // namespace bound
