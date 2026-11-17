#pragma once

#include "core/mempool.h"
#include "core/validation.h"
#include "net/addrman.h"
#include "net/banman.h"
#include "net/headerssync.h"
#include "net/protocol.h"
#include "net/socket.h"
#include "util/version.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace bound {

using PeerId = std::uint64_t;
inline constexpr PeerId kNoPeer = 0;  // never a real peer's id: "from this node itself"

struct PeerConfig {
    std::uint16_t listen_port = 0;  // announced in version; 0 means we take no connections
    std::string user_agent = bound::user_agent("boundd");
    std::size_t max_headers = kMaxHeaders;  // per headers reply we send (tests use small batches)
    std::size_t max_blocks_in_flight = 16;  // per peer
    std::int64_t handshake_timeout = 60;    // seconds
    std::int64_t ping_interval = 120;
    std::int64_t ping_timeout = 60;
    std::int64_t block_timeout = 60;            // a requested block that takes longer drops the peer
    std::size_t send_buffer_limit = 1 << 20;    // stop serving blocks to a peer with this much unsent
    std::size_t max_txs_in_flight = 100;        // transactions asked of one peer at a time
    std::int64_t tx_timeout = 60;               // then another peer that announced it is asked
    // Hardening: headers are stored only for a chain with at least our tip's work less this many
    // blocks' worth (a day), or the params' minimum_chain_work. Below that they go through presync.
    int antidos_blocks = 1440;
    HeadersSyncParams headers_sync;
    // Milestone 8. A peer's messages aren't processed while it has send_buffer_limit of replies
    // unsent, and aren't read off its socket while it has this much unprocessed: asking costs a
    // peer as much as answering costs us.
    std::size_t receive_backlog_limit = 5 << 20;
    // "Initial sync" (no mining, no transaction fetching) means all three: we know of more work
    // than our tip has, the tip is older than max_tip_age, and a block arrived within
    // sync_patience. Without the last two, anyone could freeze the network's miners by
    // announcing headers and never sending the blocks.
    std::int64_t max_tip_age = 24 * 60 * 60;
    std::int64_t sync_patience = 120;
    // Home nodes (after milestone 8). A node that takes connections tells its outbound peers where
    // to reach it, as Core's AdvertiseLocal does: at an `external` address (--externalip; port 0
    // means listen_port) if it has one, otherwise where most of them say they see it. Once after
    // each handshake, then on average once per self_announce_interval, so the address stays fresh
    // in everyone's address book and newcomers keep finding it.
    std::vector<NetAddress> external;
    std::int64_t self_announce_interval = 24 * 60 * 60;
};

// An address this node may be reachable at from outside (getnetworkinfo's localaddresses).
struct LocalAddress {
    NetAddress addr;
    bool manual = false;    // given with --externalip
    std::size_t peers = 0;  // outbound peers that see us at it
};

struct PeerInfo {
    PeerId id = 0;
    NetAddress addr;
    bool inbound = false;
    bool ready = false;  // the handshake is done
    std::int32_t start_height = 0;
    std::string user_agent;
    std::size_t blocks_in_flight = 0;
};

// Milestone 6: what nodes say to each other (Bitcoin's PeerManager, net_processing.cpp), without
// any sockets: the transport feeds in each peer's bytes and sends what gets queued, so tests can
// wire nodes together in memory. Every call must come from the one thread that owns the chainstate.
//
// A connection starts with version and verack both ways. Then each side asks for the other's headers
// (getheaders, headers), validates them, and fetches the blocks missing on the path to the best
// header, at most max_blocks_in_flight at a time per peer (getdata, block). New tips are announced
// as headers. Peers learn addresses through getaddr and addr. A peer that breaks a rule is cut off.
//
// Milestone 7: transactions. Each one accepted into the mempool is announced by wtxid (inv) to the
// peers that don't have it yet, and fetched by those that want it (getdata, tx). A new peer is
// told about the whole mempool. A transaction whose parent hasn't arrived yet waits as an orphan.
//
// Hardening: a peer that breaks a rule is discouraged for a day as well as cut off (BanMan),
// unless it's on this machine or a --connect peer. Headers of a chain with too little work to
// matter go through presync (HeadersSync) before any of them is stored.
class PeerManager final : public ChainListener {
public:
    using Logger = std::function<void(const std::string&)>;

    // Follows `chain` (as a ChainListener) while it exists; the others must outlive it too.
    PeerManager(Chainstate& chain, Mempool& mempool, AddrMan& addrman, BanMan& banman, PeerConfig config,
                std::uint64_t seed, Logger log = {});
    ~PeerManager() override;
    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    // A new connection. On outbound ones we speak first. `manual`: a --connect peer, never discouraged.
    void add_peer(PeerId id, const NetAddress& addr, bool inbound, std::int64_t now, bool manual = false);
    // Drops every peer at this IP on the next tick (a new ban).
    void disconnect_ip(const NetAddress& addr);
    void remove_peer(PeerId id);
    // Bytes from the peer. false means it broke the protocol: disconnect it.
    bool receive(PeerId id, ByteSpan data, std::int64_t now);
    // Bytes queued for the peer. After sending n of them, call sent(id, n).
    ByteSpan pending(PeerId id) const;
    void sent(PeerId id, std::size_t n);
    // Handshake and ping timeouts, stalled downloads, new block requests. Returns the peers to drop.
    std::vector<PeerId> tick(std::int64_t now);
    // A block made here (our miner's): validated like any other and announced if it's the new tip.
    BlockAcceptResult submit_block(const Block& block, std::int64_t now);
    // Milestone 7. A transaction made here (the wallet's, sendrawtransaction): into the mempool,
    // and announced if it gets in.
    MempoolAccept submit_transaction(const Transaction& tx, std::int64_t now);

    void set_listen_port(std::uint16_t port) { config_.listen_port = port; }
    std::vector<PeerInfo> peers() const;
    // Addresses that turned out to be this node itself, never to be dialled again.
    const std::set<NetAddress>& self_addresses() const { return self_addresses_; }
    std::size_t orphan_count() const { return orphans_.size(); }
    // The work a chain needs before its headers are stored without presync.
    arith_uint256 anti_dos_threshold() const;
    std::size_t headers_syncs() const;  // peers in presync or redownload now
    // Milestone 8. Still catching up with the network for the first time (see PeerConfig): the
    // miner waits, and transactions aren't fetched.
    bool initial_sync(std::int64_t now) const;
    // The transport should stop reading from this peer for now (receive_backlog_limit).
    bool read_paused(PeerId id) const;
    // Where we might be reachable: the --externalip addresses, then each public address outbound
    // peers report seeing us at (their version's addr_you, with our listening port), the most
    // reported first. Empty when we take no connections. The first one is what we advertise.
    std::vector<LocalAddress> local_addresses() const;

    // ChainListener: remembers confirmed transactions, so announcements of them aren't fetched.
    void block_connected(const Block& block, const BlockIndex& node) override;

private:
    struct Peer {
        explicit Peer(const Magic& magic) : reader(magic) {}

        NetAddress addr;
        bool inbound = false;
        bool manual = false;      // a --connect peer
        bool disconnect = false;  // drop it on the next tick
        std::int64_t connected_at = 0;
        MessageReader reader;
        Bytes out;                 // queued bytes; out[out_pos..] are still unsent
        std::size_t out_pos = 0;
        bool got_version = false;
        bool ready = false;        // got verack after version
        VersionMsg version;        // theirs
        std::uint64_t version_nonce = 0;  // the nonce in the version we sent them
        double addr_tokens = 1;           // addresses we'll still take from them (a token bucket)
        std::int64_t addr_refilled = 0;
        std::int64_t last_ping = 0;
        std::optional<std::uint64_t> ping_nonce;  // a ping without its pong yet
        std::deque<InvItem> to_serve;              // getdata waiting for room in `out`
        std::map<uint256, std::int64_t> in_flight;  // blocks we asked them for, and when
        std::set<uint256> lacks;                   // blocks they answered notfound for
        const BlockIndex* best_known = nullptr;    // the most-work block we know they have
        int unconnecting_headers = 0;
        bool answered_getaddr = false;
        std::int64_t next_self_announce = 0;  // outbound: when to tell it our address again (0: once ready)
        // Milestone 7: transactions, all by wtxid.
        std::unordered_set<uint256> known_txs;       // they have it, or we told them about it
        std::vector<uint256> tx_announce;            // to put in our next inv to them
        std::vector<uint256> tx_wanted;              // they announced it and we may ask for it
        std::map<uint256, std::int64_t> tx_in_flight;  // asked of them, and when
        std::unique_ptr<HeadersSync> headers_sync;     // their low-work chain, being checked
    };
    struct Orphan {
        Transaction tx;
        PeerId from = kNoPeer;
        std::int64_t expires = 0;
    };
    // The blocks still to fetch on the way to one header, oldest first.
    struct DownloadPath {
        std::vector<const BlockIndex*> blocks;
        std::size_t done = 0;  // blocks[..done] have arrived
    };

    void send(Peer& peer, std::string_view cmd, ByteSpan payload);
    void send_version(Peer& peer, std::int64_t now);
    void send_getheaders(Peer& peer, const BlockIndex& from);
    bool misbehaving(Peer& peer, const std::string& why);
    void log(const Peer& peer, const std::string& text) const;
    void note_known(Peer& peer, const BlockIndex& node);

    bool process_input(PeerId id, Peer& peer, std::int64_t now);
    bool handle(PeerId id, Peer& peer, const Message& m, std::int64_t now);
    bool on_version(Peer& peer, const Message& m, std::int64_t now);
    bool on_verack(PeerId id, Peer& peer, std::int64_t now);
    bool on_getheaders(Peer& peer, const Message& m);
    bool on_headers(Peer& peer, const Message& m, std::int64_t now);
    bool continue_headers_sync(Peer& peer, std::span<const BlockHeader> headers, std::int64_t now);
    bool on_getdata(Peer& peer, const Message& m);
    bool on_block(Peer& peer, const Message& m, std::int64_t now);
    bool on_notfound(Peer& peer, const Message& m);
    bool on_getaddr(Peer& peer);
    bool on_addr(PeerId id, Peer& peer, const Message& m, std::int64_t now);
    bool on_inv(Peer& peer, const Message& m);
    bool on_tx(PeerId id, Peer& peer, const Message& m, std::int64_t now);

    void relay_address(const AddrEntry& entry, PeerId source);
    void announce_self(std::int64_t now);
    void after_changes(std::int64_t now);
    void announce_tip();
    void request_blocks(std::int64_t now);
    DownloadPath& download_path(const BlockIndex* target);
    void serve(Peer& peer);
    void forget_request(const uint256& hash);

    // Milestone 7.
    MempoolAccept process_tx(const Transaction& tx, PeerId from, std::int64_t now);
    bool have_tx(const uint256& wtxid) const;
    void mark_known(Peer& peer, const uint256& wtxid);
    void relay_tx(const uint256& wtxid, PeerId source);
    void announce_txs();
    void request_txs(std::int64_t now);
    // Stops waiting for `wtxid` from `peer`. Only one peer is asked at a time, so this frees it
    // to be asked of another: peer.tx_in_flight holds it exactly when tx_requested_ maps it to peer.
    void forget_tx_request(Peer& peer, const uint256& wtxid);
    void add_orphan(const Transaction& tx, PeerId from, std::int64_t now);
    void resolve_orphans(const uint256& parent_txid, std::int64_t now);
    void retry_orphans(std::int64_t now);
    void remember_reject(const uint256& wtxid);

    Chainstate& chain_;
    Mempool& mempool_;
    AddrMan& addrman_;
    BanMan& banman_;
    std::int64_t now_ = 0;  // the time of the call being handled, for misbehaving()
    PeerConfig config_;
    std::mt19937_64 rng_;
    Logger log_;
    std::map<PeerId, Peer> peers_;
    std::set<NetAddress> self_addresses_;
    std::optional<NetAddress> advertised_;  // the address announce_self last sent, for the log

    std::map<uint256, PeerId> requested_;                  // every block in flight, and from whom
    std::map<const BlockIndex*, DownloadPath> paths_;      // by the header each leads to; see request_blocks
    const BlockIndex* announced_ = nullptr;                // the tip we last announced
    std::int64_t last_progress_ = 0;                       // when the tip last moved (or the node started)

    std::map<uint256, PeerId> tx_requested_;        // every transaction in flight, and from whom
    std::map<uint256, Orphan> orphans_;              // by wtxid
    std::unordered_set<uint256> recent_rejects_;     // wtxids the mempool refused since the tip moved
    std::unordered_set<uint256> recent_confirmed_;   // wtxids in recent blocks
    const BlockIndex* tx_tip_ = nullptr;             // the tip when orphans were last retried
};

}  // namespace bound
