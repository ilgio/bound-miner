#include "core/consensus.h"
#include "core/miner.h"
#include "core/validation.h"
#include "net/addrman.h"
#include "net/peerman.h"
#include "net/protocol.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

using namespace bound;

namespace {

PeerConfig listening_on(PeerConfig config, std::uint16_t port) {
    config.listen_port = port;
    return config;
}

// A node without sockets: its own chain (in memory), mempool, address book and peer manager.
struct TestNode {
    TestNode(const ConsensusParams& params, std::uint8_t number, PeerConfig config = {})
        : chain(params), mempool(chain), addrman(number), addr(NetAddress::ipv4(10, 0, 0, number, 18444)),
          peers(chain, mempool, addrman, banman, listening_on(config, addr.port), number) {}

    Chainstate chain;
    Mempool mempool;
    AddrMan addrman;
    BanMan banman;
    NetAddress addr;  // where it listens
    PeerManager peers;
};

// Moves bytes between TestNodes the way TCP would, so whole networks can be tested in memory.
class TestNet {
public:
    // `from` dials `to`, which sees it coming from an ephemeral port, as with a real accept().
    void connect(TestNode& from, TestNode& to) {
        Link link{&from, next_id_++, &to, next_id_++};
        NetAddress seen = from.addr;
        seen.port = static_cast<std::uint16_t>(40000 + link.to_id);
        to.peers.add_peer(link.to_id, seen, true, kTestNow);
        from.peers.add_peer(link.from_id, to.addr, false, kTestNow);
        links_.push_back(link);
    }

    // Delivers everything queued, both ways, until nobody has anything left to say.
    void settle(std::int64_t now = kTestNow) {
        for (int round = 0; round < 100000; ++round) {
            bool moved = false;
            for (Link& link : links_) {
                if (link.up) moved |= deliver(link, *link.from, link.from_id, *link.to, link.to_id, now);
                if (link.up) moved |= deliver(link, *link.to, link.to_id, *link.from, link.from_id, now);
            }
            if (!moved) return;
        }
        ADD_FAILURE() << "the network never went quiet";
    }

    bool up(const TestNode& a, const TestNode& b) const {
        for (const Link& link : links_) {
            if (link.up && ((link.from == &a && link.to == &b) || (link.from == &b && link.to == &a))) return true;
        }
        return false;
    }

private:
    struct Link {
        TestNode* from;
        PeerId from_id;
        TestNode* to;
        PeerId to_id;
        bool up = true;
    };

    // One side's queued bytes to the other; a receiver that refuses them hangs up the link.
    static bool deliver(Link& link, TestNode& src, PeerId src_id, TestNode& dst, PeerId dst_id, std::int64_t now) {
        const ByteSpan out = src.peers.pending(src_id);
        if (out.empty()) return false;
        const Bytes bytes(out.begin(), out.end());
        src.peers.sent(src_id, bytes.size());
        if (!dst.peers.receive(dst_id, bytes, now)) {
            link.up = false;
            src.peers.remove_peer(src_id);
            dst.peers.remove_peer(dst_id);
        }
        return true;
    }

    std::vector<Link> links_;
    PeerId next_id_ = 1;
};

// The far end of an inbound connection, driven by hand: for peers that misbehave.
class RawPeer {
public:
    enum class Kind { Inbound, Manual, Outbound };  // Manual: a --connect peer the node dialled

    // Inbound from `addr` unless said otherwise. Several on one node need different ids.
    explicit RawPeer(TestNode& node, NetAddress addr = NetAddress::ipv4(10, 9, 9, 9, 40000), Kind kind = Kind::Inbound,
                     PeerId id = 1000)
        : node_(node), magic_(node.chain.params().message_start), addr_(addr), id_(id) {
        node_.peers.add_peer(id_, addr, kind == Kind::Inbound, kTestNow, kind == Kind::Manual);
    }

    PeerId id() const { return id_; }
    const NetAddress& addr() const { return addr_; }
    // false: the node hung up.
    bool send(std::string_view cmd, ByteSpan payload = {}, std::int64_t now = kTestNow) {
        return send_raw(frame_message(magic_, cmd, payload), now);
    }
    bool send_raw(ByteSpan bytes, std::int64_t now = kTestNow) { return node_.peers.receive(id_, bytes, now); }
    bool handshake() {
        VersionMsg v;
        v.nonce = 12345;
        v.user_agent = "/raw/";
        return send(command::kVersion, encode_version(v)) && send(command::kVerack);
    }
    // Everything the node has sent us so far.
    std::vector<Message> received() {
        MessageReader reader(magic_);
        const ByteSpan out = node_.peers.pending(id_);
        reader.feed(out);
        node_.peers.sent(id_, out.size());
        std::vector<Message> messages;
        while (std::optional<Message> m = reader.next()) messages.push_back(*m);
        return messages;
    }

private:
    TestNode& node_;
    Magic magic_;
    NetAddress addr_;
    PeerId id_;
};

bool has_command(const std::vector<Message>& messages, std::string_view cmd) {
    return std::any_of(messages.begin(), messages.end(), [&](const Message& m) { return m.command == cmd; });
}

Block greedy_block(const Chainstate& chain) {
    Block block = block_on(chain.tip(), chain.params(), test_key("mallory").public_key(), kTestGenesisTime + 60);
    block.txs[0].outputs[0].amount += 1;  // more than the subsidy
    block.header.merkle_root = block.compute_merkle_root();
    block.header = mine(block.header, chain.params()).value();
    return block;
}

class P2PTest : public ::testing::Test {
protected:
    const ConsensusParams params = easy_params();
    const PublicKey alice = test_key("alice").public_key();
    const PublicKey bob = test_key("bob").public_key();
    TestNet net;
};

}  // namespace

TEST_F(P2PTest, HandshakeCompletes) {
    TestNode a(params, 1);
    TestNode b(params, 2);
    net.connect(a, b);
    net.settle();
    ASSERT_EQ(a.peers.peers().size(), 1u);
    EXPECT_TRUE(a.peers.peers()[0].ready);
    EXPECT_FALSE(a.peers.peers()[0].inbound);
    ASSERT_EQ(b.peers.peers().size(), 1u);
    EXPECT_TRUE(b.peers.peers()[0].ready);
    EXPECT_TRUE(b.peers.peers()[0].inbound);
    EXPECT_EQ(b.peers.peers()[0].user_agent, user_agent("boundd"));
}

TEST_F(P2PTest, NewNodeSyncsTheWholeChain) {
    TestNode a(params, 1);
    TestNode b(params, 2);
    mine_blocks(a.chain, alice, 30);
    net.connect(b, a);
    net.settle();
    EXPECT_EQ(b.chain.height(), 30);
    EXPECT_EQ(b.chain.tip().hash, a.chain.tip().hash);
    EXPECT_TRUE(b.chain.utxos() == a.chain.utxos());
}

// Many small batches of headers and a narrow download window still get there.
TEST_F(P2PTest, SyncsInSmallBatches) {
    PeerConfig small;
    small.max_headers = 4;
    small.max_blocks_in_flight = 2;
    TestNode a(params, 1, small);
    TestNode b(params, 2, small);
    mine_blocks(a.chain, alice, 25);
    net.connect(b, a);
    net.settle();
    EXPECT_EQ(b.chain.tip().hash, a.chain.tip().hash);
}

TEST_F(P2PTest, NewBlocksTravelAlongALine) {
    TestNode a(params, 1);
    TestNode b(params, 2);
    TestNode c(params, 3);
    net.connect(a, b);
    net.connect(b, c);
    net.settle();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(a.peers.submit_block(next_block(a.chain, alice), kTestNow).accepted());
        net.settle();
        EXPECT_EQ(c.chain.tip().hash, a.chain.tip().hash) << "block " << i + 1;
    }
}

// Two nodes that mined apart: on meeting, the one with less work reorganises to the other's chain.
TEST_F(P2PTest, NodesAgreeOnTheMostWork) {
    TestNode a(params, 1);
    TestNode b(params, 2);
    mine_blocks(a.chain, alice, 5);
    mine_blocks(b.chain, bob, 8);
    net.connect(a, b);
    net.settle();
    EXPECT_EQ(a.chain.height(), 8);
    EXPECT_EQ(a.chain.tip().hash, b.chain.tip().hash);
    EXPECT_TRUE(a.chain.utxos() == b.chain.utxos());
    EXPECT_EQ(a.chain.tree_size(), 1u + 5 + 8);  // Alice's blocks are kept, on a side branch
}

TEST_F(P2PTest, AnInvalidBlockCutsThePeerOff) {
    TestNode node(params, 1);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    EXPECT_FALSE(raw.send(command::kBlock, greedy_block(node.chain).serialize()));
    EXPECT_EQ(node.chain.height(), 0);
}

TEST_F(P2PTest, GarbageCutsThePeerOff) {
    TestNode node(params, 1);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    Bytes wrong_network = frame_message({1, 2, 3, 4}, command::kPing, encode_nonce(1));
    EXPECT_FALSE(raw.send_raw(wrong_network));
}

TEST_F(P2PTest, TheHandshakeComesFirst) {
    TestNode node(params, 1);
    RawPeer early(node);
    EXPECT_FALSE(early.send(command::kGetAddr));

    TestNode other(params, 2);
    RawPeer twice(other);
    ASSERT_TRUE(twice.handshake());
    VersionMsg v;
    v.nonce = 1;
    EXPECT_FALSE(twice.send(command::kVersion, encode_version(v)));
}

TEST_F(P2PTest, ANodeThatDialsItselfNoticesAndRemembers) {
    TestNode a(params, 1);
    a.addrman.add(a.addr, kTestNow);  // as if a peer had told us about our own address
    net.connect(a, a);
    net.settle();
    EXPECT_FALSE(net.up(a, a));
    EXPECT_TRUE(a.peers.self_addresses().contains(a.addr));
    EXPECT_EQ(a.addrman.find(a.addr), nullptr);
}

// Each node tells the hub its listening port; the hub passes new addresses on and answers getaddr.
TEST_F(P2PTest, AddressesSpread) {
    TestNode hub(params, 1);
    TestNode b(params, 2);
    TestNode c(params, 3);
    TestNode d(params, 4);
    net.connect(b, hub);
    net.settle();
    net.connect(c, hub);
    net.settle();
    EXPECT_NE(hub.addrman.find(b.addr), nullptr);
    EXPECT_NE(hub.addrman.find(c.addr), nullptr);
    EXPECT_NE(b.addrman.find(c.addr), nullptr);  // passed on when c arrived

    net.connect(d, hub);
    net.settle();
    EXPECT_NE(d.addrman.find(b.addr), nullptr);  // from the hub's getaddr reply
    EXPECT_NE(d.addrman.find(c.addr), nullptr);
}

TEST_F(P2PTest, SilentPeersAreDropped) {
    TestNode node(params, 1);
    const RawPeer raw(node);  // connects and never says a word
    EXPECT_TRUE(node.peers.tick(kTestNow + 60).empty());
    EXPECT_EQ(node.peers.tick(kTestNow + 61), std::vector<PeerId>{raw.id()});
}

TEST_F(P2PTest, PeersMustAnswerPings) {
    TestNode node(params, 1);
    RawPeer polite(node);
    ASSERT_TRUE(polite.handshake());
    polite.received();
    EXPECT_TRUE(node.peers.tick(kTestNow + 120).empty());  // sends a ping
    std::optional<std::uint64_t> nonce;
    for (const Message& m : polite.received()) {
        if (m.command == command::kPing) nonce = decode_nonce(m.payload);
    }
    ASSERT_TRUE(nonce.has_value());
    ASSERT_TRUE(polite.send(command::kPong, encode_nonce(*nonce)));
    EXPECT_TRUE(node.peers.tick(kTestNow + 200).empty());

    TestNode other(params, 2);
    RawPeer rude(other);
    ASSERT_TRUE(rude.handshake());
    EXPECT_TRUE(other.peers.tick(kTestNow + 120).empty());  // ping, never answered
    EXPECT_EQ(other.peers.tick(kTestNow + 181), std::vector<PeerId>{rude.id()});
}

// A peer that announces blocks and then never sends them is dropped, freeing them for others.
TEST_F(P2PTest, PeersThatDontDeliverAreDropped) {
    TestNode source(params, 2);
    mine_blocks(source.chain, alice, 3);
    TestNode node(params, 1);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    std::vector<BlockHeader> headers;
    for (int h = 1; h <= 3; ++h) headers.push_back(source.chain.at(h).header);
    ASSERT_TRUE(raw.send(command::kHeaders, encode_headers(headers)));
    EXPECT_TRUE(has_command(raw.received(), command::kGetData));
    EXPECT_TRUE(node.peers.tick(kTestNow + 60).empty());
    EXPECT_EQ(node.peers.tick(kTestNow + 61), std::vector<PeerId>{raw.id()});
}

TEST_F(P2PTest, AnInvalidHeaderCutsThePeerOff) {
    TestNode node(params, 1);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    BlockHeader bad = greedy_block(node.chain).header;
    bad.bits = 0x207ffffe;  // not the difficulty the rules give it
    bad = mine(bad, params).value();
    const std::vector<BlockHeader> headers{bad};
    EXPECT_FALSE(raw.send(command::kHeaders, encode_headers(headers)));
}

// Hardening: discouragement.

TEST_F(P2PTest, ARuleBreakerIsDiscouraged) {
    TestNode node(params, 1);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    EXPECT_FALSE(raw.send(command::kBlock, greedy_block(node.chain).serialize()));
    EXPECT_TRUE(node.banman.is_discouraged(raw.addr(), kTestNow));
    EXPECT_FALSE(node.banman.is_discouraged(raw.addr(), kTestNow + BanMan::kDiscourageTime));
}

// On one machine every node shares 127.0.0.1, and a --connect peer was chosen by hand: both are
// only cut off.
TEST_F(P2PTest, LocalAndManualPeersAreOnlyCutOff) {
    TestNode node(params, 1);
    RawPeer local(node, NetAddress::ipv4(127, 0, 0, 1, 40000));
    ASSERT_TRUE(local.handshake());
    EXPECT_FALSE(local.send(command::kBlock, greedy_block(node.chain).serialize()));
    EXPECT_FALSE(node.banman.is_discouraged(local.addr(), kTestNow));

    TestNode other(params, 2);
    RawPeer manual(other, NetAddress::ipv4(10, 8, 8, 8, 18444), RawPeer::Kind::Manual);
    ASSERT_TRUE(manual.handshake());
    EXPECT_FALSE(manual.send(command::kBlock, greedy_block(other.chain).serialize()));
    EXPECT_FALSE(other.banman.is_discouraged(manual.addr(), kTestNow));
}

TEST_F(P2PTest, ABanDisconnectsOnTheNextTick) {
    TestNode node(params, 1);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    node.peers.disconnect_ip(NetAddress::ipv4(10, 9, 9, 9, 1));  // any port
    EXPECT_EQ(node.peers.tick(kTestNow), std::vector<PeerId>{raw.id()});
}

// Hardening: headers presync. With antidos_blocks = 5, a chain needs our tip's work less 5
// blocks' worth before its headers are stored.

namespace {

PeerConfig presync_config() {
    PeerConfig config;
    config.antidos_blocks = 5;
    config.max_headers = 4;
    config.headers_sync = {2, 6};
    return config;
}

std::vector<BlockHeader> headers_of(const Chainstate& chain, int from, int to) {
    std::vector<BlockHeader> headers;
    for (int h = from; h <= to; ++h) headers.push_back(chain.at(h).header);
    return headers;
}

}  // namespace

TEST_F(P2PTest, ALowWorkForkIsNeverStored) {
    TestNode node(params, 1, presync_config());
    mine_blocks(node.chain, alice, 30);
    Chainstate fork(params);
    mine_blocks(fork, bob, 20);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    raw.received();
    ASSERT_TRUE(raw.send(command::kHeaders, encode_headers(headers_of(fork, 1, 20))));
    EXPECT_EQ(node.peers.headers_syncs(), 1u);
    EXPECT_TRUE(has_command(raw.received(), command::kGetHeaders));  // "go on"
    ASSERT_TRUE(raw.send(command::kHeaders, encode_headers({})));      // "that's all"
    EXPECT_EQ(node.peers.headers_syncs(), 0u);
    EXPECT_EQ(node.chain.tree_size(), 31u);
    EXPECT_FALSE(node.banman.is_discouraged(raw.addr(), kTestNow));  // a weak chain isn't an offence
}

// Two nodes that mined apart. The one behind syncs to the other through presync, in batches of 4;
// the one ahead checks the other's chain and never stores a header of it.
TEST_F(P2PTest, PresyncLetsTheBetterChainThrough) {
    TestNode a(params, 1, presync_config());
    TestNode b(params, 2, presync_config());
    mine_blocks(a.chain, alice, 30);
    mine_blocks(b.chain, bob, 40);
    net.connect(a, b);
    net.settle();
    EXPECT_EQ(a.chain.tip().hash, b.chain.tip().hash);
    EXPECT_EQ(a.chain.tree_size(), 1u + 30 + 40);
    EXPECT_EQ(b.chain.tree_size(), 1u + 40);
    EXPECT_EQ(a.peers.headers_syncs(), 0u);
    EXPECT_EQ(b.peers.headers_syncs(), 0u);
}

TEST_F(P2PTest, PresyncCatchesASwappedChain) {
    PeerConfig config = presync_config();
    config.headers_sync = {1, 100};  // every header committed, nothing stored before the end
    TestNode node(params, 1, config);
    mine_blocks(node.chain, alice, 30);
    Chainstate shown(params);
    Chainstate swapped(params);
    mine_blocks(shown, bob, 40);
    mine_blocks(swapped, test_key("carol").public_key(), 40);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    // A first batch with too little work starts presync. (A batch that carries the work on its own
    // is stored straight away, as in Core.)
    ASSERT_TRUE(raw.send(command::kHeaders, encode_headers(headers_of(shown, 1, 4))));
    ASSERT_EQ(node.peers.headers_syncs(), 1u);
    ASSERT_TRUE(raw.send(command::kHeaders, encode_headers(headers_of(shown, 5, 40))));    // the work is there
    ASSERT_TRUE(raw.send(command::kHeaders, encode_headers(headers_of(swapped, 1, 40))));  // "here it is again"
    EXPECT_EQ(node.peers.headers_syncs(), 0u);
    EXPECT_EQ(node.chain.tree_size(), 31u);
}

TEST_F(P2PTest, AnInvalidHeaderInPresyncCutsThePeerOff) {
    TestNode node(params, 1, presync_config());
    mine_blocks(node.chain, alice, 30);
    Chainstate fork(params);
    mine_blocks(fork, bob, 10);
    std::vector<BlockHeader> headers = headers_of(fork, 1, 10);
    headers[9].bits = 0x207ffffe;
    headers[9] = mine(headers[9], params).value();
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    EXPECT_FALSE(raw.send(command::kHeaders, encode_headers(headers)));
    EXPECT_TRUE(node.banman.is_discouraged(raw.addr(), kTestNow));
    EXPECT_EQ(node.chain.tree_size(), 31u);
}

// Milestone 8: a node among strangers.

// Blocks nobody asked for are cheap to make at a testnet's difficulty and big to store. One is
// kept only if its chain has the work to matter and it would at least match our tip.
TEST_F(P2PTest, UnrequestedBlocksNeedWork) {
    TestNode node(params, 1, presync_config());
    mine_blocks(node.chain, alice, 30);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());

    // On genesis, 30 blocks of work behind: neither the block nor its header is kept.
    const Block weak = block_on(node.chain.at(0), params, bob, kTestGenesisTime + 61);
    ASSERT_TRUE(raw.send(command::kBlock, weak.serialize()));
    EXPECT_EQ(node.chain.find(weak.hash()), nullptr);

    // A rival to block 29, one block of work behind the tip: the header is kept, the block isn't.
    const Block behind = block_on(node.chain.at(28), params, bob, kTestGenesisTime + 60 * 29 + 1);
    ASSERT_TRUE(raw.send(command::kBlock, behind.serialize()));
    ASSERT_NE(node.chain.find(behind.hash()), nullptr);
    EXPECT_FALSE(node.chain.find(behind.hash())->have_data);

    // A rival to the tip itself, with the same work: kept, though the tip stays (first seen wins).
    const Block rival = block_on(node.chain.at(29), params, bob, kTestGenesisTime + 60 * 30 + 1);
    ASSERT_TRUE(raw.send(command::kBlock, rival.serialize()));
    ASSERT_NE(node.chain.find(rival.hash()), nullptr);
    EXPECT_TRUE(node.chain.find(rival.hash())->have_data);
    EXPECT_EQ(node.chain.height(), 30);

    // And one that extends the tip is simply the next block.
    const Block next = next_block(node.chain, bob);
    ASSERT_TRUE(raw.send(command::kBlock, next.serialize()));
    EXPECT_EQ(node.chain.tip().hash, next.hash());
    EXPECT_EQ(node.chain.tree_size(), 31u + 3);
}

// Headers whose blocks never come must not keep a node from the chains its other peers have, nor
// stop its miner.
TEST_F(P2PTest, WithheldBlocksDontStallTheNode) {
    PeerConfig config;
    config.max_tip_age = 2'000'000'000;  // the test chain is dated years before the test clock: call that recent
    TestNode node(params, 1, config);
    mine_blocks(node.chain, alice, 30);
    // The withholder's chain: ours plus three blocks we'll only ever see the headers of.
    Chainstate withheld(params);
    for (int h = 1; h <= 30; ++h) ASSERT_TRUE(withheld.accept_block(node.chain.block_at(h), kTestNow).accepted());
    mine_blocks(withheld, test_key("mallory").public_key(), 3);
    // An honest peer's: ours plus two.
    TestNode honest(params, 2);
    for (int h = 1; h <= 30; ++h) ASSERT_TRUE(honest.chain.accept_block(node.chain.block_at(h), kTestNow).accepted());
    mine_blocks(honest.chain, bob, 2);

    RawPeer withholder(node);
    ASSERT_TRUE(withholder.handshake());
    ASSERT_TRUE(withholder.send(command::kHeaders, encode_headers(headers_of(withheld, 31, 33))));
    ASSERT_EQ(node.chain.best_header().hash, withheld.tip().hash);
    EXPECT_TRUE(has_command(withholder.received(), command::kGetData));  // asked for, never sent

    net.connect(honest, node);
    net.settle();
    EXPECT_EQ(node.chain.tip().hash, honest.chain.tip().hash);        // two blocks on...
    EXPECT_EQ(node.chain.best_header().hash, withheld.tip().hash);    // ...though the best header is elsewhere
    EXPECT_FALSE(node.peers.initial_sync(kTestNow));                  // and the miner may mine
    EXPECT_EQ(node.peers.tick(kTestNow + 61), std::vector<PeerId>{withholder.id()});  // the withholder times out
}

// "Initial sync" pauses the miner and transaction fetching, so it has to end on its own.
TEST_F(P2PTest, InitialSyncIsBounded) {
    Chainstate ahead(params);
    mine_blocks(ahead, alice, 3);

    TestNode node(params, 1);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    EXPECT_FALSE(node.peers.initial_sync(kTestNow));  // nothing better known
    ASSERT_TRUE(raw.send(command::kHeaders, encode_headers(headers_of(ahead, 1, 3))));
    EXPECT_TRUE(node.peers.initial_sync(kTestNow));         // behind, with an old tip, and just started
    EXPECT_TRUE(node.peers.initial_sync(kTestNow + 120));
    EXPECT_FALSE(node.peers.initial_sync(kTestNow + 121));  // no block for two minutes: stop waiting
    ASSERT_TRUE(raw.send(command::kBlock, ahead.block_at(1).serialize(), kTestNow + 200));
    ASSERT_EQ(node.chain.height(), 1);
    EXPECT_TRUE(node.peers.initial_sync(kTestNow + 320));   // a block arrived: wait again
    EXPECT_FALSE(node.peers.initial_sync(kTestNow + 321));

    // With a recent tip it's never an initial sync, whatever headers have been heard of.
    PeerConfig config;
    config.max_tip_age = 2'000'000'000;
    TestNode steady(params, 2, config);
    RawPeer other(steady);
    ASSERT_TRUE(other.handshake());
    ASSERT_TRUE(other.send(command::kHeaders, encode_headers(headers_of(ahead, 1, 3))));
    EXPECT_FALSE(steady.peers.initial_sync(kTestNow));
}

// A megabyte of getheaders mustn't queue gigabytes of headers: replies are only made as fast as
// the peer reads them, and no more is read from it meanwhile.
TEST_F(P2PTest, RepliesWaitForTheReader) {
    PeerConfig config;
    config.send_buffer_limit = 10'000;
    config.receive_backlog_limit = 2'000;
    TestNode node(params, 1, config);
    mine_blocks(node.chain, alice, 30);  // so each headers reply is 2.4 kB
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    raw.received();

    const Bytes ask = frame_message(params.message_start, command::kGetHeaders, encode_getheaders({{params.genesis_hash}, uint256{}}));
    Bytes flood;
    for (int i = 0; i < 100; ++i) flood.insert(flood.end(), ask.begin(), ask.end());
    ASSERT_TRUE(raw.send_raw(flood));
    EXPECT_LT(node.peers.pending(raw.id()).size(), 12'500u);  // 5 replies, not 100
    EXPECT_TRUE(node.peers.read_paused(raw.id()));

    std::size_t replies = 0;
    for (int round = 0; round < 200; ++round) {
        const std::vector<Message> got = raw.received();  // reading lets the node answer some more
        if (got.empty()) break;
        replies += static_cast<std::size_t>(
            std::count_if(got.begin(), got.end(), [](const Message& m) { return m.command == command::kHeaders; }));
    }
    EXPECT_EQ(replies, 100u);
    EXPECT_FALSE(node.peers.read_paused(raw.id()));
}

// "I don't have that block" only counts for blocks we asked that peer for. Otherwise a peer could
// fill our memory with hashes, or talk us out of asking it later.
TEST_F(P2PTest, NotfoundForBlocksNeverAskedIsIgnored) {
    TestNode node(params, 1);
    Chainstate ahead(params);
    mine_blocks(ahead, alice, 2);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    raw.received();
    const std::vector<InvItem> none{{InvType::Block, ahead.at(1).hash}};
    ASSERT_TRUE(raw.send(command::kNotFound, encode_inventory(none)));
    ASSERT_TRUE(raw.send(command::kHeaders, encode_headers(headers_of(ahead, 1, 2))));
    bool asked = false;
    for (const Message& m : raw.received()) {
        if (m.command != command::kGetData) continue;
        for (const InvItem& item : decode_inventory(m.payload)) asked |= item.hash == ahead.at(1).hash;
    }
    EXPECT_TRUE(asked);
}

// Core's token bucket: one address every 10 seconds from each peer, on top of a getaddr reply.
TEST_F(P2PTest, AddressFloodsAreRateLimited) {
    TestNode node(params, 1);
    RawPeer raw(node);  // inbound: one address to begin with
    ASSERT_TRUE(raw.handshake());
    const auto batch = [](std::uint8_t subnet) {
        std::vector<AddrEntry> entries;
        for (std::uint8_t i = 1; i <= 10; ++i) {
            entries.push_back({static_cast<std::uint32_t>(kTestNow), kNodeNetwork, NetAddress::ipv4(10, subnet, 0, i, 18444)});
        }
        return entries;
    };
    ASSERT_TRUE(raw.send(command::kAddr, encode_addresses(batch(1))));
    EXPECT_EQ(node.addrman.size(), 1u);
    ASSERT_TRUE(raw.send(command::kAddr, encode_addresses(batch(2)), kTestNow + 50));  // 50 seconds: 5 more
    EXPECT_EQ(node.addrman.size(), 6u);
}

// Every connection gets its own nonce. A peer that sends one of ours back can only make us forget
// the address we dialled to reach it, never one it names.
TEST_F(P2PTest, AReplayedNonceBlacklistsOnlyTheReplayer) {
    TestNode node(params, 1);
    const NetAddress attacker = NetAddress::ipv4(10, 6, 6, 6, 18444);
    const NetAddress victim = NetAddress::ipv4(10, 7, 7, 7, 18444);
    RawPeer dialled(node, attacker, RawPeer::Kind::Outbound, 1001);
    std::optional<std::uint64_t> nonce;
    for (const Message& m : dialled.received()) {
        if (m.command == command::kVersion) nonce = decode_version(m.payload).nonce;
    }
    ASSERT_TRUE(nonce.has_value());

    RawPeer replay(node, NetAddress::ipv4(10, 6, 6, 6, 40001), RawPeer::Kind::Inbound, 1002);
    VersionMsg v;
    v.nonce = *nonce;
    v.addr_you = victim;  // "the address you dialled was the victim's"
    EXPECT_FALSE(replay.send(command::kVersion, encode_version(v)));
    EXPECT_FALSE(node.peers.self_addresses().contains(victim));
    EXPECT_TRUE(node.peers.self_addresses().contains(attacker));

    RawPeer normal(node, NetAddress::ipv4(10, 5, 5, 5, 40002), RawPeer::Kind::Inbound, 1003);
    EXPECT_TRUE(normal.handshake());  // a nonce that was never ours is just a peer's
}

// Home nodes: a node tells the peers it dialled where to reach it, as Core's AdvertiseLocal does.

namespace {

// Answers the node's version on a connection it made (or took), saying where we see it.
bool handshake_seeing(RawPeer& raw, const NetAddress& seen_as, std::uint64_t nonce) {
    VersionMsg v;
    v.nonce = nonce;
    v.user_agent = "/raw/";
    v.addr_you = seen_as;
    return raw.send(command::kVersion, encode_version(v)) && raw.send(command::kVerack);
}

std::vector<AddrEntry> addresses_in(const std::vector<Message>& messages) {
    std::vector<AddrEntry> all;
    for (const Message& m : messages) {
        if (m.command != command::kAddr) continue;
        const std::vector<AddrEntry> entries = decode_addresses(m.payload);
        all.insert(all.end(), entries.begin(), entries.end());
    }
    return all;
}

}  // namespace

TEST_F(P2PTest, AdvertisesWhereItsPeersSeeIt) {
    TestNode node(params, 1);  // listens on port 18444
    RawPeer out(node, NetAddress::ipv4(8, 8, 4, 4, 18444), RawPeer::Kind::Outbound, 1001);
    ASSERT_TRUE(handshake_seeing(out, NetAddress::ipv4(1, 2, 3, 4, 51000), 777));  // its ephemeral port
    out.received();
    node.peers.tick(kTestNow);
    const std::vector<AddrEntry> told = addresses_in(out.received());
    ASSERT_EQ(told.size(), 1u);
    EXPECT_EQ(told[0].addr, NetAddress::ipv4(1, 2, 3, 4, 18444));  // with the port it listens on
    EXPECT_EQ(told[0].time, static_cast<std::uint32_t>(kTestNow));

    node.peers.tick(kTestNow + 60);
    EXPECT_TRUE(addresses_in(out.received()).empty());  // not again a minute later
    node.peers.tick(kTestNow + 10 * 24 * 60 * 60);
    EXPECT_EQ(addresses_in(out.received()).size(), 1u);  // but again within days, to stay fresh

    // Our own address, passed back to us, isn't something to dial.
    const std::vector<AddrEntry> echo{{static_cast<std::uint32_t>(kTestNow), kNodeNetwork, NetAddress::ipv4(1, 2, 3, 4, 18444)}};
    ASSERT_TRUE(out.send(command::kAddr, encode_addresses(echo)));
    EXPECT_EQ(node.addrman.find(NetAddress::ipv4(1, 2, 3, 4, 18444)), nullptr);
}

// Only peers we dialled get a say (anyone can connect in and claim anything), and only a public
// address is worth advertising.
TEST_F(P2PTest, OnlyPublicAddressesFromOutboundPeersCount) {
    TestNode node(params, 1);
    RawPeer in(node, NetAddress::ipv4(8, 8, 8, 8, 40000), RawPeer::Kind::Inbound, 1001);
    ASSERT_TRUE(handshake_seeing(in, NetAddress::ipv4(1, 2, 3, 4, 18444), 701));
    RawPeer lan(node, NetAddress::ipv4(8, 8, 4, 4, 18444), RawPeer::Kind::Outbound, 1002);
    ASSERT_TRUE(handshake_seeing(lan, NetAddress::ipv4(192, 168, 1, 6, 51000), 702));
    in.received();
    lan.received();
    node.peers.tick(kTestNow);
    EXPECT_TRUE(node.peers.local_addresses().empty());
    EXPECT_TRUE(addresses_in(in.received()).empty());
    EXPECT_TRUE(addresses_in(lan.received()).empty());
}

TEST_F(P2PTest, TheMostReportedAddressWins) {
    TestNode node(params, 1);
    RawPeer a(node, NetAddress::ipv4(8, 8, 4, 1, 18444), RawPeer::Kind::Outbound, 1001);
    RawPeer b(node, NetAddress::ipv4(8, 8, 4, 2, 18444), RawPeer::Kind::Outbound, 1002);
    RawPeer c(node, NetAddress::ipv4(8, 8, 4, 3, 18444), RawPeer::Kind::Outbound, 1003);
    ASSERT_TRUE(handshake_seeing(a, NetAddress::ipv4(9, 9, 9, 9, 50001), 801));  // a liar, or a stale view
    ASSERT_TRUE(handshake_seeing(b, NetAddress::ipv4(1, 2, 3, 4, 50002), 802));
    ASSERT_TRUE(handshake_seeing(c, NetAddress::ipv4(1, 2, 3, 4, 50003), 803));
    const std::vector<LocalAddress> local = node.peers.local_addresses();
    ASSERT_EQ(local.size(), 2u);
    EXPECT_EQ(local[0].addr, NetAddress::ipv4(1, 2, 3, 4, 18444));
    EXPECT_EQ(local[0].peers, 2u);
    EXPECT_FALSE(local[0].manual);
    EXPECT_EQ(local[1].addr, NetAddress::ipv4(9, 9, 9, 9, 18444));
    a.received();
    node.peers.tick(kTestNow);
    const std::vector<AddrEntry> told = addresses_in(a.received());
    ASSERT_EQ(told.size(), 1u);
    EXPECT_EQ(told[0].addr, NetAddress::ipv4(1, 2, 3, 4, 18444));
}

// --externalip: the address port forwarding points at, which peers can't know (here the router
// forwards its port 28333 to ours).
TEST_F(P2PTest, ExternalIpComesFirst) {
    PeerConfig config;
    config.external = {NetAddress::ipv4(5, 6, 7, 8, 28333), NetAddress::ipv4(1, 2, 3, 4, 0)};
    TestNode node(params, 1, config);
    RawPeer out(node, NetAddress::ipv4(8, 8, 4, 4, 18444), RawPeer::Kind::Outbound, 1001);
    ASSERT_TRUE(handshake_seeing(out, NetAddress::ipv4(1, 2, 3, 4, 51000), 901));
    const std::vector<LocalAddress> local = node.peers.local_addresses();
    ASSERT_EQ(local.size(), 2u);
    EXPECT_EQ(local[0].addr, NetAddress::ipv4(5, 6, 7, 8, 28333));
    EXPECT_TRUE(local[0].manual);
    EXPECT_EQ(local[1].addr, NetAddress::ipv4(1, 2, 3, 4, 18444));  // port 0: the one we listen on
    EXPECT_TRUE(local[1].manual);
    EXPECT_EQ(local[1].peers, 1u);
    out.received();
    node.peers.tick(kTestNow);
    const std::vector<AddrEntry> told = addresses_in(out.received());
    ASSERT_EQ(told.size(), 1u);
    EXPECT_EQ(told[0].addr, NetAddress::ipv4(5, 6, 7, 8, 28333));
}

TEST_F(P2PTest, ANodeThatDoesntListenKeepsQuiet) {
    PeerConfig config;
    config.external = {NetAddress::ipv4(5, 6, 7, 8, 18444)};
    TestNode node(params, 1, config);
    node.peers.set_listen_port(0);  // --nolisten
    RawPeer out(node, NetAddress::ipv4(8, 8, 4, 4, 18444), RawPeer::Kind::Outbound, 1001);
    ASSERT_TRUE(handshake_seeing(out, NetAddress::ipv4(1, 2, 3, 4, 51000), 951));
    out.received();
    node.peers.tick(kTestNow);
    EXPECT_TRUE(node.peers.local_addresses().empty());
    EXPECT_TRUE(addresses_in(out.received()).empty());
}

// A known address is passed on again once someone vouches for it half a day later than we last
// heard, so a node's daily announcement crosses the whole network; anything sooner stops here.
TEST_F(P2PTest, FreshAnnouncementsOfKnownAddressesTravel) {
    TestNode node(params, 1);
    RawPeer watcher(node, NetAddress::ipv4(8, 8, 4, 4, 18444), RawPeer::Kind::Outbound, 1001);
    ASSERT_TRUE(handshake_seeing(watcher, NetAddress::ipv4(10, 0, 0, 1, 51000), 961));
    RawPeer source(node, NetAddress::ipv4(8, 8, 8, 8, 40000), RawPeer::Kind::Inbound, 1002);
    ASSERT_TRUE(handshake_seeing(source, NetAddress::ipv4(10, 0, 0, 1, 18444), 962));
    watcher.received();
    const NetAddress home = NetAddress::ipv4(4, 4, 4, 4, 18333);
    const auto announce = [&](std::int64_t now) {
        const std::vector<AddrEntry> one{{static_cast<std::uint32_t>(now), kNodeNetwork, home}};
        EXPECT_TRUE(source.send(command::kAddr, encode_addresses(one), now));
        return addresses_in(watcher.received()).size();
    };
    EXPECT_EQ(announce(kTestNow), 1u);                // new: passed on
    EXPECT_EQ(announce(kTestNow + 60 * 60), 0u);      // an hour later: old news
    EXPECT_EQ(announce(kTestNow + 13 * 60 * 60), 1u);  // half a day on: news again
}

// Milestone 7: transaction relay.

namespace {

class TxRelayTest : public P2PTest {
protected:
    const KeyPair alice_key = test_key("alice");

    // 101 blocks paying Alice, so the coinbases of blocks 1 and 2 are spendable in the next one.
    void fund(TestNode& n) const { mine_blocks(n.chain, alice, 101); }
    // Alice's coinbase of block `height`, 5 BOUND, paid to Bob less `fee`.
    Transaction payment(const TestNode& n, int height, Amount fee = 1000) const {
        return spend({coinbase_at(n.chain, height)}, {{5 * kCoin - fee, bob}}, alice_key);
    }
};

}  // namespace

TEST_F(TxRelayTest, TransactionsTravelAlongALine) {
    TestNode a(params, 1);
    TestNode b(params, 2);
    TestNode c(params, 3);
    fund(a);
    net.connect(a, b);
    net.connect(b, c);
    net.settle();
    ASSERT_EQ(c.chain.height(), 101);

    const Transaction tx = payment(a, 1);
    ASSERT_TRUE(a.peers.submit_transaction(tx, kTestNow).accepted());
    net.settle();
    EXPECT_TRUE(b.mempool.contains(tx.txid()));
    EXPECT_TRUE(c.mempool.contains(tx.txid()));
}

// A node that connects late is told about what's waiting, even before it has the blocks those
// transactions spend from: they wait as orphans until it has caught up.
TEST_F(TxRelayTest, ANewNodeLearnsTheMempool) {
    TestNode a(params, 1);
    TestNode b(params, 2);
    fund(a);
    const Transaction first = payment(a, 1);
    const Transaction second = payment(a, 2);
    ASSERT_TRUE(a.peers.submit_transaction(first, kTestNow).accepted());
    ASSERT_TRUE(a.peers.submit_transaction(second, kTestNow).accepted());
    net.connect(b, a);
    net.settle();
    EXPECT_EQ(b.chain.height(), 101);
    EXPECT_TRUE(b.mempool.contains(first.txid()));
    EXPECT_TRUE(b.mempool.contains(second.txid()));
    EXPECT_EQ(b.peers.orphan_count(), 0u);
}

TEST_F(TxRelayTest, AChildBeforeItsParentWaitsAsAnOrphan) {
    TestNode node(params, 1);
    fund(node);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    const Transaction parent = payment(node, 1);
    const Transaction child = spend({{parent.txid(), 0}}, {{5 * kCoin - 3000, alice}}, test_key("bob"));

    ASSERT_TRUE(raw.send(command::kTx, child.serialize()));
    EXPECT_FALSE(node.mempool.contains(child.txid()));
    EXPECT_EQ(node.peers.orphan_count(), 1u);
    ASSERT_TRUE(raw.send(command::kTx, parent.serialize()));
    EXPECT_TRUE(node.mempool.contains(parent.txid()));
    EXPECT_TRUE(node.mempool.contains(child.txid()));
    EXPECT_EQ(node.peers.orphan_count(), 0u);
}

// A peer a block ahead may send a transaction spending a coinbase that's mature on its chain but
// not yet on ours. It waits for that block instead of being refused for good.
TEST_F(TxRelayTest, ATransactionTooEarlyWaitsForTheBlock) {
    TestNode node(params, 1);
    mine_blocks(node.chain, alice, 100);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    const Transaction early = payment(node, 2);  // block 2's coinbase: spendable in #102, not #101
    ASSERT_TRUE(raw.send(command::kTx, early.serialize()));
    EXPECT_FALSE(node.mempool.contains(early.txid()));
    EXPECT_EQ(node.peers.orphan_count(), 1u);
    ASSERT_TRUE(node.peers.submit_block(next_block(node.chain, alice), kTestNow).accepted());
    EXPECT_TRUE(node.mempool.contains(early.txid()));
    EXPECT_EQ(node.peers.orphan_count(), 0u);
}

TEST_F(TxRelayTest, AnInvalidTransactionCutsThePeerOff) {
    TestNode node(params, 1);
    fund(node);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    Transaction forged = payment(node, 1);
    forged.signatures[0][0] ^= 1;
    EXPECT_FALSE(raw.send(command::kTx, forged.serialize()));
}

// Too low a fee isn't the peer's fault: its mempool may have other rules. It just isn't kept.
TEST_F(TxRelayTest, ACheapTransactionIsRefusedQuietly) {
    TestNode node(params, 1);
    fund(node);
    RawPeer raw(node);
    ASSERT_TRUE(raw.handshake());
    const Transaction free = payment(node, 1, 0);
    EXPECT_TRUE(raw.send(command::kTx, free.serialize()));
    EXPECT_FALSE(node.mempool.contains(free.txid()));
}

TEST_F(TxRelayTest, AReplacementSpreadsAndEvictsTheOriginal) {
    TestNode a(params, 1);
    TestNode b(params, 2);
    fund(a);
    net.connect(a, b);
    net.settle();
    const Transaction original = payment(a, 1, 1000);
    ASSERT_TRUE(a.peers.submit_transaction(original, kTestNow).accepted());
    net.settle();
    ASSERT_TRUE(b.mempool.contains(original.txid()));

    const Transaction better = payment(a, 1, 5000);
    const MempoolAccept r = a.peers.submit_transaction(better, kTestNow);
    ASSERT_TRUE(r.accepted()) << r.reason;
    EXPECT_EQ(r.replaced, std::vector<uint256>{original.txid()});
    net.settle();
    EXPECT_TRUE(b.mempool.contains(better.txid()));
    EXPECT_FALSE(b.mempool.contains(original.txid()));
}

TEST_F(TxRelayTest, AMinedBlockEmptiesEveryMempool) {
    TestNode a(params, 1);
    TestNode b(params, 2);
    fund(a);
    net.connect(a, b);
    net.settle();
    const Transaction tx = payment(a, 1);
    ASSERT_TRUE(a.peers.submit_transaction(tx, kTestNow).accepted());
    net.settle();
    ASSERT_EQ(b.mempool.size(), 1u);

    ASSERT_TRUE(a.peers.submit_block(next_block(a.chain, alice, a.mempool.select(1'000'000).txs), kTestNow).accepted());
    net.settle();
    EXPECT_EQ(b.chain.tip().hash, a.chain.tip().hash);
    EXPECT_EQ(a.mempool.size(), 0u);
    EXPECT_EQ(b.mempool.size(), 0u);
    EXPECT_NE(b.chain.utxos().find({tx.txid(), 0}), nullptr);
}
