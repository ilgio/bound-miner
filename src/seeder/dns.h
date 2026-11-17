#pragma once

#include "net/socket.h"
#include "util/bytes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace bound {

// Milestone 8: just enough of DNS (RFC 1035) for a seeder, an authoritative server for one name
// whose A and AAAA records are the addresses of nodes that are up right now. Bitcoin's seeders
// (sipa's bitcoin-seeder) do the same.
//
// A packet is a 12-byte header (id, flags, four counts), then the question (a name as
// length-prefixed labels, a type and a class), then resource records. Replies here repeat the
// question, set the "authoritative" flag, and point each answer's name back at the question's
// (a 2-byte compression pointer) to save space.

namespace dns {
inline constexpr std::uint16_t kTypeA = 1;
inline constexpr std::uint16_t kTypeNS = 2;
inline constexpr std::uint16_t kTypeSOA = 6;
inline constexpr std::uint16_t kTypeAAAA = 28;
inline constexpr std::uint16_t kTypeOPT = 41;
inline constexpr std::uint16_t kTypeANY = 255;
inline constexpr std::uint16_t kClassIN = 1;
inline constexpr std::uint16_t kClassANY = 255;
inline constexpr std::uint8_t kNoError = 0;
inline constexpr std::uint8_t kFormErr = 1;
inline constexpr std::uint8_t kNXDomain = 3;
inline constexpr std::uint8_t kNotImp = 4;
inline constexpr std::uint8_t kRefused = 5;
inline constexpr std::size_t kMaxPlainReply = 512;  // what fits any resolver, without EDNS
inline constexpr std::size_t kMaxReply = 1232;      // what we'll send to one that says it takes more
}  // namespace dns

struct DnsQuery {
    std::uint16_t id = 0;
    std::uint8_t opcode = 0;         // 0: a standard query
    bool recursion_desired = false;  // copied into the reply, as the RFC asks
    std::string name;                // lowercase and dotted: "seed.example.org"
    Bytes raw_name;                  // as it arrived, in wire form: resolvers randomise the letter case
                                     // and expect it back unchanged
    std::uint16_t type = 0;
    std::uint16_t klass = 0;
    bool edns = false;               // it carried an OPT record (EDNS, RFC 6891)
    std::uint16_t udp_size = 512;    // the reply size it says it can take
};

// The header and the one question of a query. nullopt for anything else: too short, a reply, not
// exactly one question, a compressed or over-long name.
std::optional<DnsQuery> parse_dns_query(ByteSpan packet);

// The zone a seeder answers for.
struct DnsZone {
    std::string host;        // the seed's name: "seed.example.org"
    std::string nameserver;  // the name server's own name, for NS and SOA: "ns.example.org"
    std::string mailbox;     // the operator's mailbox in DNS form: "hostmaster.example.org"
    std::uint32_t serial = 1;
    std::uint32_t ttl = 300;        // how long resolvers may keep the addresses
    std::uint32_t zone_ttl = 3600;  // and the NS and SOA records
};

// The reply to a query:
//  - for the zone's name: A records (the IPv4 `addresses`), AAAA (the others), NS or SOA, by the
//    type asked; any other type gets an empty answer with the SOA record as its authority;
//  - for a name under the zone's: "no such name";
//  - for any other name: "refused".
// As many addresses as fit the reply size the query allows; the caller picks which to offer.
Bytes build_dns_reply(const DnsQuery& query, const DnsZone& zone, std::span<const NetAddress> addresses);
// A reply that is only a header: for a packet with an id but no usable question.
Bytes build_dns_error(std::uint16_t id, std::uint8_t rcode);

// "seed.Example.org." -> "seed.example.org"; nullopt if it isn't a valid host name (labels of 1 to
// 63 letters, digits, hyphens or underscores; at most 253 characters).
std::optional<std::string> normalize_dns_name(std::string_view name);

}  // namespace bound
