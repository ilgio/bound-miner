#pragma once

#include "net/protocol.h"
#include "net/socket.h"
#include "util/bytes.h"
#include "util/version.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace bound {

struct CrawlerOptions {
    Magic magic{};                     // the network's message start
    std::uint16_t default_port = 0;    // DNS can't carry a port: only nodes listening on this one are handed out
    std::size_t max_probes = 64;       // connections open at once
    std::int64_t connect_timeout = 10;
    std::int64_t probe_timeout = 30;   // from starting to connect to the end of the handshake
    std::int64_t addr_wait = 10;       // after the handshake, for the reply to getaddr
    std::int64_t recheck = 30 * 60;    // a node that answered is tried again after this long
    bool allow_private = false;        // also probe and keep private and local addresses (tests, a LAN)
    std::string user_agent = bound::user_agent("bound-seeder");
    std::size_t max_nodes = 50'000;
};

struct NodeRecord {
    NetAddress addr;
    std::int64_t first_seen = 0;
    std::int64_t last_try = 0;
    std::int64_t last_success = 0;
    std::uint32_t failures = 0;   // in a row
    std::uint32_t tries = 0;
    std::uint32_t successes = 0;
    double reliability = 0;       // a moving average of how its probes went: 1 if every one answered
    std::uint32_t version = 0;    // from its version message, at its last success
    std::uint64_t services = 0;
    std::int32_t height = 0;
    std::string user_agent;

    friend bool operator==(const NodeRecord&, const NodeRecord&) = default;
};

// Milestone 8: a DNS seeder's crawler (the job of sipa's bitcoin-seeder). It keeps every node
// address it has heard of and keeps probing them:
//   connect -> version and verack both ways -> getaddr -> wait for the addr reply -> hang up.
// Each probe that completes the handshake is a success; the addresses in the replies are probed in
// turn, so the whole network is found from one starting point. good() is what a DNS reply offers.
//
// A PollClient: its sockets share the seeder's poll loop with the DNS server.
class Crawler final : public PollClient {
public:
    Crawler(CrawlerOptions options, std::uint64_t seed);

    // An address to try. false if it's known already, or unusable (port 0, or private without
    // allow_private), or the list is full.
    bool add(const NetAddress& addr, std::int64_t now);
    // A starting point the operator named (--node). Probed even if it's private or local, such as
    // the node on the seeder's own machine, whose getaddr reply leads to the rest of the network;
    // but like every private address it's never handed out. Milestone 14.
    bool add_start(const NetAddress& addr, std::int64_t now);
    // Ends probes that took too long, forgets hopeless addresses, starts the probes that are due.
    void tick(std::int64_t now);
    void add_poll_items(std::vector<PollItem>& items) override;
    void handle_poll(std::span<const PollItem> items, std::int64_t now) override;

    // Nodes to hand out, in no order: listening on the default port, a full node of a protocol
    // version we speak, whose last probe answered (within two rechecks), that answers at least
    // half the time, and whose chain is within a day of the network's (the median height of the
    // nodes that answered in the last day: one node claiming a huge height can't exclude the rest).
    std::vector<NetAddress> good(std::int64_t now) const;

    const NodeRecord* find(const NetAddress& addr) const;
    std::size_t size() const { return nodes_.size(); }
    std::size_t probing() const { return probes_.size(); }

    // seeder.dat: a version byte, a compact_size count, then each record.
    Bytes serialize() const;
    // Replaces the records with those in `data`. Throws DeserializeError if it's malformed.
    void load(ByteSpan data);

private:
    struct Probe {
        explicit Probe(const Magic& magic) : reader(magic) {}

        Socket socket;
        NetAddress addr;
        MessageReader reader;
        Bytes out;
        std::size_t out_pos = 0;
        std::int64_t started = 0;
        bool connected = false;
        bool got_version = false;
        bool got_verack = false;
        std::int64_t asked_addr = 0;  // when the handshake completed and getaddr went out
        VersionMsg version;
        bool done = false;
    };

    void finish(Probe& probe, bool success, std::int64_t now);
    bool usable(const NetAddress& addr) const;
    void send(Probe& probe, std::string_view command, ByteSpan payload) const;
    void read(Probe& probe, std::int64_t now);
    bool handle(Probe& probe, const Message& m, std::int64_t now);  // false: the probe failed
    void flush(Probe& probe);

    CrawlerOptions options_;
    std::mt19937_64 rng_;
    std::map<NetAddress, NodeRecord> nodes_;
    std::map<std::uint64_t, Probe> probes_;
    std::vector<std::uint64_t> polled_;  // the probes add_poll_items listed, in that order
    std::uint64_t next_id_ = 1;
    std::int64_t last_cleanup_ = 0;
};

}  // namespace bound
