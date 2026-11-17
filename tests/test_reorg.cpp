#include "core/consensus.h"
#include "core/miner.h"
#include "core/pow.h"
#include "core/validation.h"
#include "core/wallet.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace bound;

namespace {

// Milestone 4: competing branches, fork choice and reorganisations.
class ReorgTest : public ::testing::Test {
protected:
    // A mined block on `parent` for `miner`, a minute per height after the test genesis. The
    // branches in these tests have different miners, so their blocks never collide.
    Block on(const BlockIndex& parent, const KeyPair& miner, std::vector<Transaction> txs = {}) const {
        const auto time = static_cast<std::uint32_t>(kTestGenesisTime + 60 * (parent.height + 1));
        return block_on(parent, params, miner.public_key(), time, std::move(txs));
    }

    // Like on(), but the coinbase claims one unit more than the subsidy: fine by every rule that
    // can be checked without the chain, invalid once connected.
    Block greedy_on(const BlockIndex& parent, const KeyPair& miner) const {
        Block block = on(parent, miner);
        block.txs[0].outputs[0].amount += 1;
        block.header.merkle_root = block.compute_merkle_root();
        block.header = mine(block.header, params).value();
        return block;
    }

    // Mines and accepts `count` blocks for `miner` on top of `parent`; returns the last one.
    const BlockIndex& extend(const BlockIndex& parent, const KeyPair& miner, int count) {
        const BlockIndex* node = &parent;
        for (int i = 0; i < count; ++i) {
            const Block block = on(*node, miner);
            if (!chain.accept_block(block, kTestNow).accepted()) throw std::logic_error("test block rejected");
            node = chain.find(block.hash());
        }
        return *node;
    }

    const ConsensusParams params = easy_params();
    Chainstate chain{params};
    const BlockIndex& genesis = chain.tip();
    const KeyPair alice = test_key("alice");
    const KeyPair bob = test_key("bob");
    const KeyPair carol = test_key("carol");
};

}  // namespace

TEST_F(ReorgTest, BranchWithMoreWorkTakesOver) {
    const BlockIndex& a2 = extend(genesis, alice, 2);
    ASSERT_EQ(&chain.tip(), &a2);

    const BlockIndex& c2 = extend(genesis, carol, 2);
    EXPECT_EQ(c2.chain_work, a2.chain_work);
    EXPECT_EQ(&chain.tip(), &a2);  // of two chains with equal work, the first one complete stays
    EXPECT_TRUE(c2.have_data);
    EXPECT_FALSE(chain.in_active_chain(c2));

    const BlockIndex& c3 = extend(c2, carol, 1);  // now Carol's branch has more work
    EXPECT_EQ(&chain.tip(), &c3);
    EXPECT_EQ(chain.height(), 3);
    EXPECT_EQ(&chain.at(1), c2.parent);
    EXPECT_FALSE(chain.in_active_chain(a2));
    EXPECT_EQ(balance(chain.utxos(), alice.public_key()), 0);
    EXPECT_EQ(balance(chain.utxos(), carol.public_key()), 15 * kCoin);
}

// After a reorg, the UTXO set must be exactly what connecting the winning branch from scratch
// gives: every disconnected block fully undone, every connected one fully applied.
TEST_F(ReorgTest, ReorgMatchesBuildingTheWinnerDirectly) {
    mine_blocks(chain, alice.public_key(), 101);
    const BlockIndex& fork = chain.tip();
    const std::optional<Transaction> pay =
        create_payment(chain.utxos(), alice, bob.public_key(), 7 * kCoin + kCoin / 2, kCoin / 1000, 102, params);
    ASSERT_TRUE(pay.has_value());
    ASSERT_TRUE(chain.accept_block(next_block(chain, alice.public_key(), {*pay}), kTestNow).accepted());
    ASSERT_EQ(balance(chain.utxos(), bob.public_key()), 7 * kCoin + kCoin / 2);

    const BlockIndex& carols_tip = extend(fork, carol, 2);
    ASSERT_EQ(&chain.tip(), &carols_tip);
    EXPECT_EQ(balance(chain.utxos(), bob.public_key()), 0);  // the payment was only in the losing block
    EXPECT_EQ(balance(chain.utxos(), alice.public_key()), 101 * 5 * kCoin);

    Chainstate direct(params);
    for (int h = 1; h <= chain.height(); ++h) ASSERT_TRUE(direct.accept_block(chain.block_at(h), kTestNow).accepted());
    EXPECT_TRUE(chain.utxos() == direct.utxos());
}

// A block that's only invalid in context (its coinbase claims too much) can sit on a side branch
// unnoticed until that branch is tried. Then it's marked invalid with everything on top of it,
// and the node goes back to the chain it came from.
TEST_F(ReorgTest, InvalidBlockOnABranchSendsTheTipBack) {
    const BlockIndex& a2 = extend(genesis, alice, 2);
    const UtxoSet before = chain.utxos();

    const BlockIndex& c1 = extend(genesis, carol, 1);
    const Block greedy = greedy_on(c1, carol);
    ASSERT_TRUE(chain.accept_block(greedy, kTestNow).accepted());  // ties with a2: stored, not tried yet
    const BlockIndex& c2 = *chain.find(greedy.hash());

    const BlockAcceptResult r = chain.accept_block(on(c2, carol), kTestNow);  // the branch pulls ahead
    EXPECT_EQ(r.header, AcceptResult::InvalidParent);
    EXPECT_FALSE(r.accepted());
    EXPECT_EQ(&chain.tip(), &a2);
    EXPECT_TRUE(chain.utxos() == before);
    EXPECT_TRUE(c2.invalid);
    EXPECT_EQ(c2.invalid_reason, ConnectResult::CoinbaseTooLarge);
    EXPECT_FALSE(c1.invalid);
}

TEST_F(ReorgTest, RemembersInvalidBlocks) {
    const Block greedy = greedy_on(genesis, alice);
    EXPECT_EQ(chain.accept_block(greedy, kTestNow).body, ConnectResult::CoinbaseTooLarge);
    EXPECT_EQ(chain.accept_block(greedy, kTestNow).body, ConnectResult::CoinbaseTooLarge);  // from memory

    // Nothing may build on it, as a header or as a block.
    const Block child = on(*chain.find(greedy.hash()), alice);
    EXPECT_EQ(chain.accept_header(child.header, kTestNow), AcceptResult::InvalidParent);
    EXPECT_EQ(chain.accept_block(child, kTestNow).header, AcceptResult::InvalidParent);
    EXPECT_EQ(chain.find(child.hash()), nullptr);
    EXPECT_EQ(chain.height(), 0);
}

// Headers first, then bodies in any order, as milestone 6's sync will deliver them. Nothing is
// connected until all of a branch's transactions are there.
TEST_F(ReorgTest, BodiesCanArriveAfterTheirHeadersInAnyOrder) {
    const BlockIndex& a1 = extend(genesis, alice, 1);
    const Block c1 = on(genesis, carol);
    ASSERT_EQ(chain.accept_header(c1.header, kTestNow), AcceptResult::Accepted);
    const Block c2 = on(*chain.find(c1.hash()), carol);
    ASSERT_EQ(chain.accept_header(c2.header, kTestNow), AcceptResult::Accepted);
    const Block c3 = on(*chain.find(c2.hash()), carol);
    ASSERT_EQ(chain.accept_header(c3.header, kTestNow), AcceptResult::Accepted);
    EXPECT_EQ(&chain.tip(), &a1);  // the headers have more work, but there's nothing to connect

    EXPECT_TRUE(chain.accept_block(c3, kTestNow).accepted());
    EXPECT_FALSE(chain.find(c3.hash())->linked);  // waits for its parents
    EXPECT_TRUE(chain.accept_block(c1, kTestNow).accepted());
    EXPECT_EQ(&chain.tip(), &a1);  // c1 alone only ties with a1
    EXPECT_TRUE(chain.accept_block(c2, kTestNow).accepted());  // links c2 and the waiting c3
    EXPECT_EQ(chain.tip().hash, c3.hash());
    EXPECT_EQ(chain.height(), 3);
}

// A block whose transactions don't match its header is turned away without blaming the header,
// so the real block still gets in.
TEST_F(ReorgTest, MismatchedBodyDoesNotTaintTheHeader) {
    const Block good = on(genesis, alice);
    Block mutated = good;
    const ByteSpan memo = bytes_of("not what the header commits to");
    mutated.txs[0].memo.assign(memo.begin(), memo.end());
    EXPECT_EQ(chain.accept_block(mutated, kTestNow).body, ConnectResult::BadBlock);
    EXPECT_EQ(chain.find(good.hash()), nullptr);
    EXPECT_TRUE(chain.accept_block(good, kTestNow).accepted());
    EXPECT_EQ(chain.accept_block(good, kTestNow).header, AcceptResult::Duplicate);

    // The same when the header is already in the tree.
    const Block next = on(chain.tip(), alice);
    ASSERT_EQ(chain.accept_header(next.header, kTestNow), AcceptResult::Accepted);
    Block empty = next;
    empty.txs.clear();
    EXPECT_EQ(chain.accept_block(empty, kTestNow).body, ConnectResult::BadBlock);
    EXPECT_FALSE(chain.find(next.hash())->invalid);
    EXPECT_TRUE(chain.accept_block(next, kTestNow).accepted());
    EXPECT_EQ(chain.height(), 2);
}

// Fork choice counts work, not blocks. With a 4-block period, Carol's first three blocks come
// four times faster than planned, so from height 4 each of hers needs four times the work, and
// her five blocks outweigh Alice's eight. (Work values computed independently in Python.)
TEST(ReorgWork, MostWorkBeatsMostBlocks) {
    ConsensusParams params = easy_params();
    params.retarget_interval = 4;
    Chainstate chain(params);
    const BlockIndex& genesis = chain.tip();
    const PublicKey alice = test_key("alice").public_key();
    const PublicKey carol = test_key("carol").public_key();

    const BlockIndex* a = &genesis;
    for (std::uint32_t i = 1; i <= 8; ++i) {
        const Block block = block_on(*a, params, alice, kTestGenesisTime + 60 * i);
        ASSERT_TRUE(chain.accept_block(block, kTestNow).accepted());
        a = chain.find(block.hash());
    }
    const BlockIndex* c = &genesis;
    for (std::uint32_t i = 1; i <= 5; ++i) {
        const Block block = block_on(*c, params, carol, kTestGenesisTime + 15 * i);
        ASSERT_TRUE(chain.accept_block(block, kTestNow).accepted());
        c = chain.find(block.hash());
    }

    EXPECT_EQ(a->chain_work, arith_uint256(18));  // 9 blocks (genesis included) of work 2
    EXPECT_EQ(c->header.bits, 0x201fffffu);
    EXPECT_EQ(c->chain_work, arith_uint256(24));  // 4 blocks of work 2, then 2 of work 8
    EXPECT_EQ(&chain.tip(), c);
    EXPECT_EQ(chain.height(), 5);
}
