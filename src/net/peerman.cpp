#include "net/peerman.h"

#include "core/chain.h"
#include "core/pow.h"
#include "util/serialize.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

namespace bound {

namespace {

constexpr std::size_t kAnnounceHeaders = 8;     // a new tip is announced with up to this many headers
constexpr int kMaxUnconnectingHeaders = 8;       // headers that don't attach to our tree, before we give up
constexpr std::int64_t kAddrFresh = 10 * 60;     // only addresses seen this recently are passed on
constexpr std::size_t kAddrRelayPeers = 2;
constexpr std::size_t kMaxTxWanted = 5000;       // announcements kept per peer, waiting to be fetched
constexpr std::size_t kMaxKnownTxs = 50000;      // per peer; forgetting only means announcing again
constexpr std::size_t kMaxOrphans = 100;
constexpr std::size_t kMaxOrphanSize = 100'000;
constexpr std::int64_t kOrphanExpiry = 20 * 60;
constexpr std::size_t kMaxRecent = 50000;        // recent_rejects_ and recent_confirmed_
constexpr double kAddrPerSecond = 0.1;           // addresses taken from one peer, beyond a getaddr reply
constexpr int kMaxUnrequestedAhead = 288;        // an unrequested block this far above our tip isn't kept (Core's)
constexpr std::int64_t kAddrRefresh = 12 * 60 * 60;  // a known address seen this much later is news again

// The decoded payload, or nullopt if it's malformed.
template <typename F>
auto decoded(F&& decode) -> std::optional<decltype(decode())> {
    try {
        return decode();
    } catch (const DeserializeError&) {
        return std::nullopt;
    }
}

const char* name(AcceptResult r) {
    switch (r) {
    case AcceptResult::Accepted: return "accepted";
    case AcceptResult::Duplicate: return "duplicate";
    case AcceptResult::BadProofOfWork: return "bad proof of work";
    case AcceptResult::UnknownParent: return "unknown parent";
    case AcceptResult::InvalidParent: return "invalid parent";
    case AcceptResult::BadDifficulty: return "wrong difficulty";
    case AcceptResult::TimeTooOld: return "time too old";
    case AcceptResult::TimeTooNew: return "time too far ahead";
    }
    return "?";
}

const char* name(ConnectResult r) {
    switch (r) {
    case ConnectResult::Ok: return "ok";
    case ConnectResult::BadBlock: return "breaks the block rules";
    case ConnectResult::BadCoinbaseHeight: return "wrong coinbase height";
    case ConnectResult::BadInputs: return "invalid or double-spent inputs";
    case ConnectResult::DuplicateOutput: return "duplicate output";
    case ConnectResult::CoinbaseTooLarge: return "coinbase pays too much";
    }
    return "?";
}

}  // namespace

PeerManager::PeerManager(Chainstate& chain, Mempool& mempool, AddrMan& addrman, BanMan& banman, PeerConfig config,
                         std::uint64_t seed, Logger log)
    : chain_(chain), mempool_(mempool), addrman_(addrman), banman_(banman), config_(std::move(config)), rng_(seed),
      log_(std::move(log)), announced_(&chain.tip()), tx_tip_(&chain.tip()) {
    chain_.add_listener(*this);
}

PeerManager::~PeerManager() { chain_.remove_listener(*this); }

void PeerManager::add_peer(PeerId id, const NetAddress& addr, bool inbound, std::int64_t now, bool manual) {
    now_ = now;
    if (last_progress_ == 0) last_progress_ = now;  // starting up counts: give the first sync time to begin
    Peer& peer = peers_.try_emplace(id, chain_.params().message_start).first->second;
    peer.addr = addr;
    peer.inbound = inbound;
    peer.manual = manual;
    peer.connected_at = now;
    peer.addr_refilled = now;
    if (!inbound) send_version(peer, now);
}

void PeerManager::disconnect_ip(const NetAddress& addr) {
    for (auto& [id, peer] : peers_) {
        if (peer.addr.ip != addr.ip) continue;
        log(peer, "disconnecting: banned");
        peer.disconnect = true;
    }
}

bool PeerManager::initial_sync(std::int64_t now) const {
    const BlockIndex& tip = chain_.tip();
    return chain_.best_header().chain_work > tip.chain_work && now - std::int64_t{tip.header.time} > config_.max_tip_age &&
           now - last_progress_ <= config_.sync_patience;
}

bool PeerManager::read_paused(PeerId id) const {
    const auto found = peers_.find(id);
    return found != peers_.end() && found->second.reader.buffered() > config_.receive_backlog_limit;
}

arith_uint256 PeerManager::anti_dos_threshold() const {
    // Core's GetAntiDoSWorkThreshold: a chain within a day's work of ours could become the best
    // one soon, so it's worth storing; one further behind has to show its work first.
    const BlockIndex& tip = chain_.tip();
    const std::optional<arith_uint256> margin =
        mul_div(block_proof(tip.header.bits), static_cast<std::uint32_t>(std::max(0, config_.antidos_blocks)), 1);
    const arith_uint256 near_tip = margin && tip.chain_work > *margin ? tip.chain_work - *margin : arith_uint256(0);
    return std::max(near_tip, chain_.params().minimum_chain_work);
}

std::size_t PeerManager::headers_syncs() const {
    return static_cast<std::size_t>(std::count_if(peers_.begin(), peers_.end(), [](const auto& p) { return p.second.headers_sync != nullptr; }));
}

void PeerManager::remove_peer(PeerId id) {
    const auto found = peers_.find(id);
    if (found == peers_.end()) return;
    for (const auto& [hash, asked] : found->second.in_flight) requested_.erase(hash);
    for (const auto& [wtxid, asked] : found->second.tx_in_flight) tx_requested_.erase(wtxid);  // others may be asked now
    peers_.erase(found);
}

bool PeerManager::receive(PeerId id, ByteSpan data, std::int64_t now) {
    now_ = now;
    const auto found = peers_.find(id);
    if (found == peers_.end()) return false;
    Peer& peer = found->second;
    peer.reader.feed(data);
    if (!process_input(id, peer, now)) return false;
    after_changes(now);
    return true;
}

// Handles the peer's waiting messages, but only while it's taking our replies: with
// send_buffer_limit unsent, the rest wait (Core's fPauseSend). Otherwise a megabyte of getheaders
// would queue gigabytes of headers. sent() carries on once the peer has read some.
bool PeerManager::process_input(PeerId id, Peer& peer, std::int64_t now) {
    try {
        while (peer.out.size() - peer.out_pos < config_.send_buffer_limit) {
            const std::optional<Message> m = peer.reader.next();
            if (!m) break;
            if (!handle(id, peer, *m, now)) return false;
        }
    } catch (const ProtocolError& e) {
        return misbehaving(peer, e.what());
    }
    return true;
}

ByteSpan PeerManager::pending(PeerId id) const {
    const auto found = peers_.find(id);
    if (found == peers_.end()) return {};
    return ByteSpan(found->second.out).subspan(found->second.out_pos);
}

void PeerManager::sent(PeerId id, std::size_t n) {
    const auto found = peers_.find(id);
    if (found == peers_.end()) return;
    Peer& peer = found->second;
    peer.out_pos += n;
    if (peer.out_pos == peer.out.size()) {
        peer.out.clear();
        peer.out_pos = 0;
    }
    serve(peer);  // room for more blocks it asked for
    // And for the messages that waited while its replies were backed up.
    if (peer.reader.buffered() > 0 && !peer.disconnect && peer.out.size() - peer.out_pos < config_.send_buffer_limit) {
        if (process_input(id, peer, now_)) {
            after_changes(now_);
        } else {
            peer.disconnect = true;  // tick() drops it
        }
    }
}

std::vector<PeerId> PeerManager::tick(std::int64_t now) {
    now_ = now;
    if (last_progress_ == 0) last_progress_ = now;
    std::vector<PeerId> drop;
    for (auto& [id, peer] : peers_) {
        if (peer.disconnect) {
            drop.push_back(id);
            continue;
        }
        if (!peer.ready) {
            if (now - peer.connected_at > config_.handshake_timeout) {
                log(peer, "disconnecting: no handshake");
                drop.push_back(id);
            }
            continue;
        }
        if (peer.ping_nonce) {
            if (now - peer.last_ping > config_.ping_timeout) {
                log(peer, "disconnecting: no pong");
                drop.push_back(id);
                continue;
            }
        } else if (now - peer.last_ping >= config_.ping_interval) {
            peer.ping_nonce = rng_();
            peer.last_ping = now;
            send(peer, command::kPing, encode_nonce(*peer.ping_nonce));
        }
        const bool stalled = std::any_of(peer.in_flight.begin(), peer.in_flight.end(),
                                         [&](const auto& request) { return now - request.second > config_.block_timeout; });
        if (stalled) {
            log(peer, "disconnecting: a block request timed out");
            drop.push_back(id);
        }
        // A transaction that doesn't come isn't worth a disconnect (it may have just been mined
        // or replaced); another peer that announced it gets asked instead.
        std::vector<uint256> late;
        for (const auto& [wtxid, asked] : peer.tx_in_flight) {
            if (now - asked > config_.tx_timeout) late.push_back(wtxid);
        }
        for (const uint256& wtxid : late) forget_tx_request(peer, wtxid);
    }
    std::erase_if(orphans_, [&](const auto& entry) { return entry.second.expires < now; });
    announce_self(now);
    after_changes(now);
    return drop;
}

std::vector<LocalAddress> PeerManager::local_addresses() const {
    std::vector<LocalAddress> list;
    if (config_.listen_port == 0) return list;
    const auto entry_for = [&](const NetAddress& addr) {
        return std::find_if(list.begin(), list.end(), [&](const LocalAddress& l) { return l.addr == addr; });
    };
    for (NetAddress addr : config_.external) {
        if (addr.port == 0) addr.port = config_.listen_port;
        if (!addr.is_unspecified() && entry_for(addr) == list.end()) list.push_back({addr, true, 0});
    }
    // Only outbound peers count: we picked them, while anyone can connect in and say anything.
    std::map<NetAddress, std::size_t> reports;
    for (const auto& [id, peer] : peers_) {
        if (peer.inbound || !peer.ready || !peer.version.addr_you.is_routable()) continue;
        NetAddress addr = peer.version.addr_you;
        addr.port = config_.listen_port;  // they saw the port we dialled from
        ++reports[addr];
    }
    const std::size_t manual = list.size();
    for (const auto& [addr, count] : reports) {
        if (const auto found = entry_for(addr); found != list.end()) {
            found->peers = count;
        } else {
            list.push_back({addr, false, count});
        }
    }
    std::stable_sort(list.begin() + static_cast<std::ptrdiff_t>(manual), list.end(),
                     [](const LocalAddress& a, const LocalAddress& b) { return a.peers > b.peers; });
    return list;
}

// Our address to each outbound peer that's due for it: right after the handshake, then about once
// a day (exponentially distributed, as in Core). It passes the news on, so the whole network keeps
// hearing that we're up. Not while we're still catching up: nobody needs a node that far behind.
void PeerManager::announce_self(std::int64_t now) {
    if (initial_sync(now)) return;
    const std::vector<LocalAddress> local = local_addresses();
    if (local.empty()) return;
    if (advertised_ != local.front().addr) {
        advertised_ = local.front().addr;
        if (log_) log_(std::format("advertising our address {}", advertised_->to_string()));
    }
    const std::array<AddrEntry, 1> one{AddrEntry{static_cast<std::uint32_t>(now), kNodeNetwork, local.front().addr}};
    std::exponential_distribution<double> wait(1.0 / static_cast<double>(std::max<std::int64_t>(1, config_.self_announce_interval)));
    for (auto& [id, peer] : peers_) {
        if (peer.inbound || !peer.ready || peer.disconnect || now < peer.next_self_announce) continue;
        send(peer, command::kAddr, encode_addresses(one));
        peer.next_self_announce = now + 1 + static_cast<std::int64_t>(wait(rng_));
    }
}

BlockAcceptResult PeerManager::submit_block(const Block& block, std::int64_t now) {
    const BlockAcceptResult result = chain_.accept_block(block, now);
    after_changes(now);
    return result;
}

MempoolAccept PeerManager::submit_transaction(const Transaction& tx, std::int64_t now) {
    const MempoolAccept result = process_tx(tx, kNoPeer, now);
    after_changes(now);
    return result;
}

void PeerManager::block_connected(const Block& block, const BlockIndex&) {
    for (const Transaction& tx : block.txs) {
        if (recent_confirmed_.size() >= kMaxRecent) recent_confirmed_.clear();
        const uint256 wtxid = tx.wtxid();
        recent_confirmed_.insert(wtxid);
        orphans_.erase(wtxid);
    }
}

std::vector<PeerInfo> PeerManager::peers() const {
    std::vector<PeerInfo> list;
    for (const auto& [id, peer] : peers_) {
        list.push_back({id, peer.addr, peer.inbound, peer.ready, peer.version.height, peer.version.user_agent,
                        peer.in_flight.size()});
    }
    return list;
}

void PeerManager::send(Peer& peer, std::string_view cmd, ByteSpan payload) {
    const Bytes frame = frame_message(chain_.params().message_start, cmd, payload);
    peer.out.insert(peer.out.end(), frame.begin(), frame.end());
}

void PeerManager::send_version(Peer& peer, std::int64_t now) {
    VersionMsg v;
    v.time = now;
    v.addr_you = peer.addr;
    v.listen_port = config_.listen_port;
    peer.version_nonce = rng_() | 1;  // fresh for every connection, and never 0 ("none sent")
    v.nonce = peer.version_nonce;
    v.user_agent = config_.user_agent;
    v.height = chain_.height();
    send(peer, command::kVersion, encode_version(v));
}

void PeerManager::send_getheaders(Peer& peer, const BlockIndex& from) {
    send(peer, command::kGetHeaders, encode_getheaders({block_locator(from), uint256{}}));
}

// One strike, as in Bitcoin Core since v22: no score to add up, since nearly every offence is one
// an honest node never commits. Peers on this machine and --connect peers are only cut off:
// discouraging an address shared by every local node would cut them all off.
bool PeerManager::misbehaving(Peer& peer, const std::string& why) {
    const bool discourage = !peer.manual && !peer.addr.is_loopback();
    if (discourage) banman_.discourage(peer.addr, now_);
    log(peer, "disconnecting: " + why + (discourage ? "; discouraged for a day" : ""));
    return false;
}

void PeerManager::log(const Peer& peer, const std::string& text) const {
    if (log_) log_(std::format("peer {}: {}", peer.addr.to_string(), text));
}

void PeerManager::note_known(Peer& peer, const BlockIndex& node) {
    if (peer.best_known == nullptr || node.chain_work > peer.best_known->chain_work) peer.best_known = &node;
}

bool PeerManager::handle(PeerId id, Peer& peer, const Message& m, std::int64_t now) {
    // version, then verack, before anything else.
    if (m.command == command::kVersion) return on_version(peer, m, now);
    if (!peer.got_version) return misbehaving(peer, "sent " + m.command + " before version");
    if (m.command == command::kVerack) return on_verack(id, peer, now);
    if (!peer.ready) return misbehaving(peer, "sent " + m.command + " before verack");

    if (m.command == command::kPing) {
        const auto nonce = decoded([&] { return decode_nonce(m.payload); });
        if (!nonce) return misbehaving(peer, "malformed ping");
        send(peer, command::kPong, encode_nonce(*nonce));
        return true;
    }
    if (m.command == command::kPong) {
        const auto nonce = decoded([&] { return decode_nonce(m.payload); });
        if (!nonce) return misbehaving(peer, "malformed pong");
        if (peer.ping_nonce == *nonce) peer.ping_nonce.reset();
        return true;
    }
    if (m.command == command::kGetHeaders) return on_getheaders(peer, m);
    if (m.command == command::kHeaders) return on_headers(peer, m, now);
    if (m.command == command::kGetData) return on_getdata(peer, m);
    if (m.command == command::kBlock) return on_block(peer, m, now);
    if (m.command == command::kNotFound) return on_notfound(peer, m);
    if (m.command == command::kGetAddr) return on_getaddr(peer);
    if (m.command == command::kAddr) return on_addr(id, peer, m, now);
    if (m.command == command::kInv) return on_inv(peer, m);
    if (m.command == command::kTx) return on_tx(id, peer, m, now);
    return true;  // unknown commands are ignored, so later versions can add their own
}

bool PeerManager::on_version(Peer& peer, const Message& m, std::int64_t now) {
    if (peer.got_version) return misbehaving(peer, "a second version");
    const auto v = decoded([&] { return decode_version(m.payload); });
    if (!v) return misbehaving(peer, "malformed version");
    // A nonce we sent on an outbound connection that hasn't answered yet, coming back on another
    // connection: that outbound connection reached this very node. Forget the address we dialled
    // (which we know ourselves; nothing the message claims is trusted). Each connection has its
    // own nonce, as in Bitcoin, so a peer that replays ours can only blacklist itself.
    for (auto& [other_id, other] : peers_) {
        if (&other == &peer || other.inbound || other.got_version || other.version_nonce != v->nonce) continue;
        if (self_addresses_.size() < 1000) self_addresses_.insert(other.addr);
        addrman_.remove(other.addr);
        other.disconnect = true;
        log(peer, "disconnecting: it's this node itself");
        return false;
    }
    if (v->version < kProtocolVersion) {
        log(peer, std::format("disconnecting: protocol version {} is too old", v->version));  // old, not bad
        return false;
    }
    peer.version = *v;
    peer.got_version = true;
    if (peer.inbound) send_version(peer, now);
    send(peer, command::kVerack, {});
    return true;
}

bool PeerManager::on_verack(PeerId id, Peer& peer, std::int64_t now) {
    if (peer.ready) return true;
    peer.ready = true;
    peer.last_ping = now;
    log(peer, std::format("connected, {}, {}, height {}", peer.inbound ? "inbound" : "outbound", peer.version.user_agent,
                          peer.version.height));
    if (!peer.inbound) {
        // An address we dialled that answered: proven reachable, so worth passing on (--connect
        // peers become known this way too).
        addrman_.add(peer.addr, now);
        addrman_.good(peer.addr, now);
        send(peer, command::kGetAddr, {});
        peer.addr_tokens += kMaxAddresses;  // room for the reply we just asked for
    } else if (peer.version.listen_port != 0) {
        // It takes connections too: worth knowing, and worth telling others about.
        NetAddress listening = peer.addr;
        listening.port = peer.version.listen_port;
        if (addrman_.add(listening, now)) relay_address({static_cast<std::uint32_t>(now), kNodeNetwork, listening}, id);
    }
    send_getheaders(peer, chain_.best_header());
    // Everything waiting for a block, so a node that just started doesn't miss what came before.
    for (const MempoolEntry* e : mempool_.entries()) peer.tx_announce.push_back(e->wtxid);
    return true;
}

bool PeerManager::on_getheaders(Peer& peer, const Message& m) {
    const auto request = decoded([&] { return decode_getheaders(m.payload); });
    if (!request) return misbehaving(peer, "malformed getheaders");
    // Start after the first locator entry on our active chain; genesis if none is.
    const BlockIndex* fork = &chain_.at(0);
    for (const uint256& hash : request->locator) {
        const BlockIndex* node = chain_.find(hash);
        if (node != nullptr && chain_.in_active_chain(*node)) {
            fork = node;
            break;
        }
    }
    std::vector<BlockHeader> headers;
    const BlockIndex* last = nullptr;
    for (int h = fork->height + 1; h <= chain_.height() && headers.size() < config_.max_headers; ++h) {
        last = &chain_.at(h);
        headers.push_back(last->header);
        if (last->hash == request->stop) break;
    }
    send(peer, command::kHeaders, encode_headers(headers));
    if (last != nullptr) note_known(peer, *last);
    return true;
}

bool PeerManager::on_headers(Peer& peer, const Message& m, std::int64_t now) {
    const auto headers = decoded([&] { return decode_headers(m.payload); });
    if (!headers) return misbehaving(peer, "malformed headers");
    if (peer.headers_sync) return continue_headers_sync(peer, *headers, now);

    // New headers on a chain that, even with them, has too little work to matter: none of it is
    // stored until presync has seen the chain reach the threshold.
    if (!headers->empty() && chain_.find(headers->back().hash()) == nullptr) {
        if (const BlockIndex* start = chain_.find(headers->front().prev_hash)) {
            const arith_uint256 threshold = anti_dos_threshold();
            arith_uint256 work = start->chain_work;
            for (const BlockHeader& h : *headers) work += block_proof(h.bits);
            if (work < threshold) {
                peer.headers_sync = std::make_unique<HeadersSync>(*start, threshold, chain_.params(), config_.headers_sync, rng_(), now);
                log(peer, std::format("headers forking at #{} with too little work to store: presync", start->height));
                return continue_headers_sync(peer, *headers, now);
            }
        }
    }

    const BlockIndex* last = nullptr;
    bool any_new = false;
    for (std::size_t i = 0; i < headers->size(); ++i) {
        const AcceptResult r = chain_.accept_header((*headers)[i], now);
        if (r == AcceptResult::Accepted || r == AcceptResult::Duplicate) {
            last = chain_.find((*headers)[i].hash());
            any_new |= r == AcceptResult::Accepted;
            continue;
        }
        if (r == AcceptResult::UnknownParent && i == 0) {
            // Most likely a new tip on a branch whose start we haven't seen: ask for the way there.
            if (++peer.unconnecting_headers > kMaxUnconnectingHeaders) return misbehaving(peer, "headers that never connect");
            send_getheaders(peer, chain_.best_header());
            return true;
        }
        if (r == AcceptResult::TimeTooNew) break;  // maybe our clock; the rest would follow it anyway
        return misbehaving(peer, std::format("an invalid header ({})", name(r)));
    }
    peer.unconnecting_headers = 0;
    if (last == nullptr) return true;
    note_known(peer, *last);
    // A reply's size doesn't say whether the peer has more (it may send smaller batches than we
    // do), so ask again after every batch that taught us something; an empty reply ends it.
    if (any_new) send_getheaders(peer, *last);
    return true;
}

bool PeerManager::continue_headers_sync(Peer& peer, std::span<const BlockHeader> headers, std::int64_t now) {
    const HeadersSync::Result r = peer.headers_sync->process(headers, now);
    if (r.invalid) {
        peer.headers_sync.reset();
        return misbehaving(peer, "an invalid header in presync (" + r.note + ")");
    }
    const BlockIndex* last = nullptr;
    for (const BlockHeader& h : r.to_store) {
        const AcceptResult a = chain_.accept_header(h, now);
        if (a == AcceptResult::TimeTooNew) {  // our clock moved on since presync: try again later
            peer.headers_sync.reset();
            return true;
        }
        if (a != AcceptResult::Accepted && a != AcceptResult::Duplicate) {
            peer.headers_sync.reset();
            return misbehaving(peer, std::format("an invalid header after presync ({})", name(a)));
        }
        last = chain_.find(h.hash());
    }
    if (last != nullptr) note_known(peer, *last);
    if (!r.note.empty()) log(peer, "presync: " + r.note);

    switch (peer.headers_sync->phase()) {
    case HeadersSync::Phase::Failed:
        peer.headers_sync.reset();  // nothing stored; a cheap chain isn't misbehaviour
        return true;
    case HeadersSync::Phase::Done:
        peer.headers_sync.reset();
        send_getheaders(peer, last != nullptr ? *last : chain_.best_header());  // the rest, the usual way
        return true;
    default:
        if (!r.locator.empty()) send(peer, command::kGetHeaders, encode_getheaders({r.locator, uint256{}}));
        return true;
    }
}

bool PeerManager::on_getdata(Peer& peer, const Message& m) {
    const auto items = decoded([&] { return decode_inventory(m.payload); });
    if (!items) return misbehaving(peer, "malformed getdata");
    if (peer.to_serve.size() + items->size() > kMaxInventory) return misbehaving(peer, "too many requests at once");
    peer.to_serve.insert(peer.to_serve.end(), items->begin(), items->end());
    return true;
}

bool PeerManager::on_block(Peer& peer, const Message& m, std::int64_t now) {
    const auto block = decoded([&] { return Block::deserialize(m.payload); });
    if (!block) return misbehaving(peer, "malformed block");
    const uint256 hash = block->hash();
    const bool requested = requested_.contains(hash);
    forget_request(hash);
    if (!requested) {
        // A block nobody asked for (Core's AcceptBlock rules). Blocks are cheap to make at a
        // testnet's difficulty and big to store, so one is only kept if its chain has the work to
        // matter and it would at least match our tip; and not if it's far ahead of what we have.
        const BlockIndex* node = chain_.find(hash);
        if (node == nullptr) {
            const BlockIndex* parent = chain_.find(block->header.prev_hash);
            if (parent == nullptr) {
                send_getheaders(peer, chain_.best_header());
                return true;
            }
            if (parent->chain_work + block_proof(block->header.bits) < anti_dos_threshold()) return true;
            const AcceptResult header = chain_.accept_header(block->header, now);
            if (header == AcceptResult::TimeTooNew) return true;
            if (header != AcceptResult::Accepted && header != AcceptResult::Duplicate) {
                return misbehaving(peer, std::format("a block with an invalid header ({})", name(header)));
            }
            node = chain_.find(hash);
        }
        if (!node->have_data && !node->invalid &&
            (node->chain_work < chain_.tip().chain_work || node->height > chain_.height() + kMaxUnrequestedAhead)) {
            return true;  // the header is kept; the block is fetched if its branch ever leads
        }
    }
    const BlockAcceptResult r = chain_.accept_block(*block, now);
    if (r.header == AcceptResult::UnknownParent) {
        send_getheaders(peer, chain_.best_header());
        return true;
    }
    if (r.header == AcceptResult::Duplicate || r.header == AcceptResult::TimeTooNew) return true;
    if (r.header != AcceptResult::Accepted) return misbehaving(peer, std::format("a block with an invalid header ({})", name(r.header)));
    if (r.body != ConnectResult::Ok) return misbehaving(peer, std::format("an invalid block ({})", name(r.body)));
    note_known(peer, *chain_.find(hash));
    return true;
}

bool PeerManager::on_notfound(Peer& peer, const Message& m) {
    const auto items = decoded([&] { return decode_inventory(m.payload); });
    if (!items) return misbehaving(peer, "malformed notfound");
    for (const InvItem& item : *items) {
        if (item.type == InvType::WitnessTx) {
            forget_tx_request(peer, item.hash);
            continue;
        }
        // Only for blocks we did ask it for: `lacks` mustn't grow with whatever a peer sends.
        if (peer.in_flight.erase(item.hash) > 0) {
            requested_.erase(item.hash);
            peer.lacks.insert(item.hash);
        }
    }
    return true;
}

bool PeerManager::on_getaddr(Peer& peer) {
    if (peer.answered_getaddr) return true;  // once per connection, so it can't map our whole table
    peer.answered_getaddr = true;
    std::vector<AddrEntry> entries;
    for (const AddrInfo& info : addrman_.sample(kMaxAddresses, now_)) {
        if (banman_.refuses(info.addr, now_)) continue;  // don't send others to who we won't talk to
        entries.push_back({static_cast<std::uint32_t>(info.last_seen), kNodeNetwork, info.addr});
    }
    send(peer, command::kAddr, encode_addresses(entries));
    return true;
}

bool PeerManager::on_addr(PeerId id, Peer& peer, const Message& m, std::int64_t now) {
    const auto entries = decoded([&] { return decode_addresses(m.payload); });
    if (!entries) return misbehaving(peer, "malformed addr");
    // A small message is news (a node announcing itself) worth passing on; a big one answers a
    // getaddr and stays here. Relaying only addresses that are new to us stops them circling.
    const bool news = entries->size() <= 10;
    // A token bucket, as in Core: one address per 10 seconds from each peer, plus the 1,000 we
    // asked for with getaddr. The rest are dropped unread, so no peer can flood the address book.
    peer.addr_tokens = std::min<double>(kMaxAddresses, peer.addr_tokens + kAddrPerSecond * static_cast<double>(std::max<std::int64_t>(0, now - peer.addr_refilled)));
    peer.addr_refilled = now;
    const std::vector<LocalAddress> local = local_addresses();
    for (AddrEntry entry : *entries) {
        if (peer.addr_tokens < 1) break;
        peer.addr_tokens -= 1;
        if (entry.time > now + kAddrFresh) entry.time = static_cast<std::uint32_t>(now - 5 * 24 * 60 * 60);  // from the future: distrust
        if (self_addresses_.contains(entry.addr)) continue;
        if (std::any_of(local.begin(), local.end(), [&](const LocalAddress& l) { return l.addr == entry.addr; })) continue;  // ours, coming back
        // An address we know is news again once it's been vouched for half a day later than we
        // last heard: that's how a node's daily announcement travels past its own peers. Only
        // news is passed on, so addresses don't circle.
        const AddrInfo* known = addrman_.find(entry.addr);
        const std::optional<std::int64_t> seen_before = known != nullptr ? std::optional(known->last_seen) : std::nullopt;
        const bool added = addrman_.add(entry.addr, entry.time);
        const bool refreshed = seen_before && std::int64_t{entry.time} >= *seen_before + kAddrRefresh;
        if ((added || refreshed) && news && entry.time + kAddrFresh >= now) relay_address(entry, id);
    }
    return true;
}

void PeerManager::relay_address(const AddrEntry& entry, PeerId source) {
    std::vector<Peer*> targets;
    for (auto& [id, peer] : peers_) {
        if (id != source && peer.ready) targets.push_back(&peer);
    }
    std::shuffle(targets.begin(), targets.end(), rng_);
    if (targets.size() > kAddrRelayPeers) targets.resize(kAddrRelayPeers);
    const std::array<AddrEntry, 1> one{entry};
    for (Peer* peer : targets) send(*peer, command::kAddr, encode_addresses(one));
}

void PeerManager::after_changes(std::int64_t now) {
    if (&chain_.tip() != tx_tip_) {
        // A new tip can make refused transactions valid (a coinbase matured, a conflict went
        // away in a reorganisation), and orphans' parents may be in it.
        tx_tip_ = &chain_.tip();
        last_progress_ = now;
        recent_rejects_.clear();
        retry_orphans(now);
    }
    announce_tip();
    announce_txs();
    request_blocks(now);
    request_txs(now);
    for (auto& [id, peer] : peers_) serve(peer);
}

void PeerManager::announce_tip() {
    const BlockIndex& tip = chain_.tip();
    if (&tip == announced_) return;
    announced_ = &tip;
    if (log_ && (tip.height % 100 == 0 || &tip == &chain_.best_header())) {
        log_(std::format("tip #{} {}", tip.height, tip.hash.to_hex()));
    }
    if (tip.height == 0) return;

    std::vector<BlockHeader> headers;
    for (int h = std::max(1, tip.height - static_cast<int>(kAnnounceHeaders) + 1); h <= tip.height; ++h) {
        headers.push_back(chain_.at(h).header);
    }
    const Bytes payload = encode_headers(headers);
    for (auto& [id, peer] : peers_) {
        if (!peer.ready) continue;
        const BlockIndex* known = peer.best_known;
        if (known != nullptr && known->height >= tip.height && known->ancestor(tip.height) == &tip) continue;  // they have it
        send(peer, command::kHeaders, payload);
        note_known(peer, tip);
    }
}

// The blocks missing on the way to `target`, worked out once per target: walking back from it is
// expensive when thousands are missing.
PeerManager::DownloadPath& PeerManager::download_path(const BlockIndex* target) {
    const auto [it, inserted] = paths_.try_emplace(target);
    if (inserted) {
        for (const BlockIndex* node = target; !node->linked; node = node->parent) {
            if (!node->have_data) it->second.blocks.push_back(node);
        }
        std::reverse(it->second.blocks.begin(), it->second.blocks.end());
    }
    return it->second;
}

// Each peer is asked for the blocks on the way to the best chain it has shown us, if that chain
// has more work than ours (Core's FindNextBlocksToDownload). Deliberately not just the one best
// header overall: a peer could announce headers and never send their blocks, and that must not
// keep us from the chains other peers offer.
void PeerManager::request_blocks(std::int64_t now) {
    const arith_uint256& tip_work = chain_.tip().chain_work;
    std::set<const BlockIndex*> in_use;
    for (auto& [id, peer] : peers_) {
        if (!peer.ready || peer.best_known == nullptr) continue;
        const BlockIndex* target = peer.best_known;
        if (target->invalid || !(target->chain_work > tip_work)) continue;
        in_use.insert(target);
        DownloadPath& path = download_path(target);
        while (path.done < path.blocks.size() && path.blocks[path.done]->have_data) ++path.done;
        std::vector<InvItem> ask;
        for (std::size_t i = path.done; i < path.blocks.size() && peer.in_flight.size() < config_.max_blocks_in_flight; ++i) {
            const BlockIndex* node = path.blocks[i];
            if (node->have_data || requested_.contains(node->hash) || peer.lacks.contains(node->hash)) continue;
            requested_.emplace(node->hash, id);
            peer.in_flight.emplace(node->hash, now);
            ask.push_back({InvType::Block, node->hash});
        }
        if (!ask.empty()) send(peer, command::kGetData, encode_inventory(ask));
    }
    std::erase_if(paths_, [&](const auto& path) { return !in_use.contains(path.first); });
}

void PeerManager::serve(Peer& peer) {
    std::vector<InvItem> missing;
    while (!peer.to_serve.empty() && peer.out.size() - peer.out_pos < config_.send_buffer_limit) {
        const InvItem item = peer.to_serve.front();
        peer.to_serve.pop_front();
        if (item.type == InvType::WitnessTx) {
            if (const MempoolEntry* e = mempool_.find_wtxid(item.hash)) {
                send(peer, command::kTx, e->tx.serialize());
                mark_known(peer, item.hash);
            } else {
                missing.push_back(item);
            }
            continue;
        }
        const BlockIndex* node = item.type == InvType::Block ? chain_.find(item.hash) : nullptr;
        if (node != nullptr && node->have_data && !node->invalid) {
            send(peer, command::kBlock, chain_.read_block(*node).serialize());
        } else {
            missing.push_back(item);
        }
    }
    if (!missing.empty()) send(peer, command::kNotFound, encode_inventory(missing));
}

void PeerManager::forget_request(const uint256& hash) {
    const auto found = requested_.find(hash);
    if (found == requested_.end()) return;
    if (const auto peer = peers_.find(found->second); peer != peers_.end()) peer->second.in_flight.erase(hash);
    requested_.erase(found);
}

// Milestone 7: transactions.

bool PeerManager::on_inv(Peer& peer, const Message& m) {
    const auto items = decoded([&] { return decode_inventory(m.payload); });
    if (!items) return misbehaving(peer, "malformed inv");
    for (const InvItem& item : *items) {
        if (item.type != InvType::WitnessTx) continue;  // blocks are announced with headers
        mark_known(peer, item.hash);
        if (!have_tx(item.hash) && peer.tx_wanted.size() < kMaxTxWanted) peer.tx_wanted.push_back(item.hash);
    }
    return true;
}

bool PeerManager::on_tx(PeerId id, Peer& peer, const Message& m, std::int64_t now) {
    const auto tx = decoded([&] { return Transaction::deserialize(m.payload); });
    if (!tx) return misbehaving(peer, "malformed tx");
    const uint256 wtxid = tx->wtxid();
    mark_known(peer, wtxid);
    forget_tx_request(peer, wtxid);
    if (have_tx(wtxid)) return true;
    const MempoolAccept result = process_tx(*tx, id, now);
    // Only a transaction that can't be valid anywhere is the peer's fault. Missing inputs, low
    // fees or a conflict can be honest: its mempool and chain aren't ours.
    if (result.result == MempoolResult::Invalid) return misbehaving(peer, "an invalid transaction (" + result.reason + ")");
    return true;
}

// Missing a parent, or spending a coinbase that's still maturing: either may change with the next
// block or transaction, especially on a node that's still syncing.
static bool waits_as_orphan(MempoolResult r) { return r == MempoolResult::MissingInputs || r == MempoolResult::Premature; }

// Offers a transaction to the mempool. Accepted: announce it, then retry the orphans waiting for
// it. Not yet valid here: keep it as an orphan (unless it's our own). Refused: don't fetch it
// again until the tip moves.
MempoolAccept PeerManager::process_tx(const Transaction& tx, PeerId from, std::int64_t now) {
    const MempoolAccept result = mempool_.accept(tx, now);
    const uint256 wtxid = tx.wtxid();
    if (result.accepted()) {
        relay_tx(wtxid, from);
        resolve_orphans(tx.txid(), now);
    } else if (waits_as_orphan(result.result)) {
        if (from != kNoPeer) add_orphan(tx, from, now);
    } else if (result.result != MempoolResult::Duplicate) {
        remember_reject(wtxid);
    }
    return result;
}

bool PeerManager::have_tx(const uint256& wtxid) const {
    return mempool_.find_wtxid(wtxid) != nullptr || orphans_.contains(wtxid) || recent_rejects_.contains(wtxid) ||
           recent_confirmed_.contains(wtxid);
}

void PeerManager::mark_known(Peer& peer, const uint256& wtxid) {
    if (peer.known_txs.size() >= kMaxKnownTxs) peer.known_txs.clear();
    peer.known_txs.insert(wtxid);
}

void PeerManager::relay_tx(const uint256& wtxid, PeerId source) {
    for (auto& [id, peer] : peers_) {
        if (id != source && peer.ready && !peer.known_txs.contains(wtxid)) peer.tx_announce.push_back(wtxid);
    }
}

// One inv per peer for everything accepted since the last pass, in the order accepted, so parents
// are announced (and fetched) before their children. Whatever left the mempool meanwhile is skipped.
void PeerManager::announce_txs() {
    for (auto& [id, peer] : peers_) {
        if (!peer.ready || peer.tx_announce.empty()) continue;
        std::vector<InvItem> items;
        for (const uint256& wtxid : peer.tx_announce) {
            if (peer.known_txs.contains(wtxid) || mempool_.find_wtxid(wtxid) == nullptr) continue;
            mark_known(peer, wtxid);
            items.push_back({InvType::WitnessTx, wtxid});
        }
        peer.tx_announce.clear();
        for (std::size_t i = 0; i < items.size(); i += kMaxInventory) {
            const std::size_t n = std::min(kMaxInventory, items.size() - i);
            send(peer, command::kInv, encode_inventory(std::span(items).subspan(i, n)));
        }
    }
}

// Asks for announced transactions we don't have, each from one peer at a time. An announcement
// already asked of another peer is kept, in case that one never answers. Nothing is asked during
// the initial sync: transactions are only checkable against the current chain.
void PeerManager::request_txs(std::int64_t now) {
    if (initial_sync(now)) return;
    for (auto& [id, peer] : peers_) {
        if (!peer.ready || peer.tx_wanted.empty()) continue;
        std::vector<InvItem> ask;
        std::vector<uint256> keep;
        for (const uint256& wtxid : peer.tx_wanted) {
            if (have_tx(wtxid)) continue;
            if (tx_requested_.contains(wtxid) || peer.tx_in_flight.size() >= config_.max_txs_in_flight) {
                keep.push_back(wtxid);
                continue;
            }
            tx_requested_.emplace(wtxid, id);
            peer.tx_in_flight.emplace(wtxid, now);
            ask.push_back({InvType::WitnessTx, wtxid});
        }
        peer.tx_wanted = std::move(keep);
        if (!ask.empty()) send(peer, command::kGetData, encode_inventory(ask));
    }
}

void PeerManager::forget_tx_request(Peer& peer, const uint256& wtxid) {
    if (peer.tx_in_flight.erase(wtxid) > 0) tx_requested_.erase(wtxid);
}

void PeerManager::add_orphan(const Transaction& tx, PeerId from, std::int64_t now) {
    if (tx.serialize().size() > kMaxOrphanSize) return;
    if (orphans_.size() >= kMaxOrphans) {
        auto victim = orphans_.begin();
        std::advance(victim, static_cast<std::ptrdiff_t>(rng_() % orphans_.size()));
        orphans_.erase(victim);
    }
    orphans_.insert_or_assign(tx.wtxid(), Orphan{tx, from, now + kOrphanExpiry});
}

// `parent_txid` just got into the mempool: try the orphans spending it, and in turn the orphans
// spending those.
void PeerManager::resolve_orphans(const uint256& parent_txid, std::int64_t now) {
    std::vector<uint256> parents{parent_txid};
    while (!parents.empty()) {
        const uint256 parent = parents.back();
        parents.pop_back();
        std::vector<uint256> ready;
        for (const auto& [wtxid, orphan] : orphans_) {
            const auto& ins = orphan.tx.inputs;
            if (std::any_of(ins.begin(), ins.end(), [&](const OutPoint& in) { return in.txid == parent; })) ready.push_back(wtxid);
        }
        for (const uint256& wtxid : ready) {
            Orphan orphan = std::move(orphans_.at(wtxid));
            orphans_.erase(wtxid);
            const MempoolAccept result = mempool_.accept(orphan.tx, now);
            if (result.accepted()) {
                relay_tx(wtxid, orphan.from);
                parents.push_back(orphan.tx.txid());
            } else if (waits_as_orphan(result.result)) {
                orphans_.emplace(wtxid, std::move(orphan));  // another parent is still missing
            } else if (result.result != MempoolResult::Duplicate) {
                remember_reject(wtxid);
            }
        }
    }
}

// Every orphan once more, after the tip moved: their parents may have been mined.
void PeerManager::retry_orphans(std::int64_t now) {
    std::vector<uint256> all;
    for (const auto& [wtxid, orphan] : orphans_) all.push_back(wtxid);
    for (const uint256& wtxid : all) {
        const auto found = orphans_.find(wtxid);
        if (found == orphans_.end()) continue;  // resolved on the way
        const MempoolAccept result = mempool_.accept(found->second.tx, now);
        if (waits_as_orphan(result.result)) continue;
        const Orphan orphan = std::move(found->second);
        orphans_.erase(found);
        if (result.accepted()) {
            relay_tx(wtxid, orphan.from);
            resolve_orphans(orphan.tx.txid(), now);
        } else if (result.result != MempoolResult::Duplicate) {
            remember_reject(wtxid);
        }
    }
}

void PeerManager::remember_reject(const uint256& wtxid) {
    if (recent_rejects_.size() >= kMaxRecent) recent_rejects_.clear();
    recent_rejects_.insert(wtxid);
}

}  // namespace bound
