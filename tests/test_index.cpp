#include "core/storage.h"
#include "core/validation.h"
#include "index/chainindex.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace bound;

namespace {

using Dump = std::vector<std::pair<Bytes, Bytes>>;

Dump dump(const KeyValueStore& db) {
    Dump all;
    db.for_each({}, [&](ByteSpan key, ByteSpan value) { all.emplace_back(Bytes(key.begin(), key.end()), Bytes(value.begin(), value.end())); });
    return all;
}

// A chain of 110 blocks paying Alice (the coinbases of blocks 1 to 11 are spendable), with an
// index following it.
class ChainIndexTest : public ::testing::Test {
protected:
    ChainIndexTest() {
        mine_blocks(chain, alice.public_key(), 110);
        index = std::make_unique<ChainIndex>(db, chain);
    }

    void mine(std::vector<Transaction> txs = {}) {
        ASSERT_TRUE(chain.accept_block(next_block(chain, alice.public_key(), std::move(txs)), kTestNow).accepted());
    }
    // Whatever happened to it, the index must hold exactly what one built from scratch would.
    void expect_matches_a_rebuild() {
        MemoryStore fresh;
        const ChainIndex rebuilt(fresh, chain);
        EXPECT_EQ(dump(db), dump(fresh));
    }
    // Alice pays Bob 2 BOUND from the coinbase of block `height`; Bob sends it on to Carol.
    Transaction pay(int height) const {
        return spend({coinbase_at(chain, height)}, {{2 * kCoin, bob.public_key()}, {3 * kCoin - 1000, alice.public_key()}}, alice);
    }
    Transaction onward(const Transaction& payment) const {
        return spend({{payment.txid(), 0}}, {{2 * kCoin - 1000, carol.public_key()}}, bob);
    }

    const ConsensusParams params = easy_params();
    const KeyPair alice = test_key("alice");
    const KeyPair bob = test_key("bob");
    const KeyPair carol = test_key("carol");
    Chainstate chain{params};
    MemoryStore db;
    std::unique_ptr<ChainIndex> index;
};

}  // namespace

TEST_F(ChainIndexTest, ANewIndexReadsTheWholeChain) {
    EXPECT_EQ(index->best_block(), chain.tip().hash);
    EXPECT_EQ(index->summary(alice.public_key()), (AddressSummary{110 * 5 * kCoin, 110 * 5 * kCoin, 0, 110, 110}));
    EXPECT_EQ(index->summary(bob.public_key()), AddressSummary{});

    const uint256 coinbase5 = chain.block_at(5).txs[0].txid();
    EXPECT_EQ(index->find_tx(coinbase5), (TxLocation{chain.at(5).hash, 0}));
    EXPECT_EQ(index->find_tx(test_hash("no such transaction")), std::nullopt);
    EXPECT_EQ(index->spender({coinbase5, 0}), std::nullopt);

    // The genesis transaction can be looked up, but its output is nobody's: it can never be spent.
    EXPECT_EQ(index->find_tx(params.genesis_coinbase.txid()), (TxLocation{params.genesis_hash, 0}));
    EXPECT_EQ(index->summary(PublicKey{}), AddressSummary{});
}

TEST_F(ChainIndexTest, FollowsPaymentsAsBlocksArrive) {
    const Transaction payment = pay(1);
    mine({payment});
    ASSERT_EQ(chain.height(), 111);

    EXPECT_EQ(index->summary(bob.public_key()), (AddressSummary{2 * kCoin, 2 * kCoin, 0, 1, 1}));
    const AddressSummary a = index->summary(alice.public_key());
    EXPECT_EQ(a.received, 110 * 5 * kCoin + (5 * kCoin + 1000) + (3 * kCoin - 1000));  // + block 111's reward and fee, + change
    EXPECT_EQ(a.sent, 5 * kCoin);
    EXPECT_EQ(a.balance, a.received - a.sent);
    EXPECT_EQ(a.txs, 112u);    // 111 coinbases and the payment
    EXPECT_EQ(a.coins, 111u);  // one coinbase spent, the change and a new coinbase gained

    const std::optional<ChainIndex::FoundTx> found = index->read_tx(payment.txid());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->tx, payment);
    EXPECT_EQ(found->block, &chain.tip());
    EXPECT_EQ(found->position, 1u);
    EXPECT_EQ(index->spender(coinbase_at(chain, 1)), payment.txid());
    EXPECT_EQ(index->spender({payment.txid(), 0}), std::nullopt);

    // History, newest first: the payment (which spent 5 and returned the change), then coinbases.
    const std::vector<AddressDelta> recent = index->history(alice.public_key(), 0, 3);
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent[0], (AddressDelta{payment.txid(), 111, 1, 3 * kCoin - 1000, 5 * kCoin}));
    EXPECT_EQ(recent[1], (AddressDelta{chain.block_at(111).txs[0].txid(), 111, 0, 5 * kCoin + 1000, 0}));
    EXPECT_EQ(recent[2].height, 110);
    // Paging: skipping two gives the same third entry first.
    const std::vector<AddressDelta> older = index->history(alice.public_key(), 2, 2);
    ASSERT_EQ(older.size(), 2u);
    EXPECT_EQ(older[0], recent[2]);
    EXPECT_EQ(older[1].height, 109);
    EXPECT_TRUE(index->history(alice.public_key(), 1000, 5).empty());
    EXPECT_TRUE(index->history(alice.public_key(), 0, 0).empty());

    const std::vector<AddressCoin> bobs = index->coins(bob.public_key(), 0, 10);
    ASSERT_EQ(bobs.size(), 1u);
    EXPECT_EQ(bobs[0], (AddressCoin{{payment.txid(), 0}, 2 * kCoin, 111, false}));
    const std::vector<AddressCoin> alices = index->coins(alice.public_key(), 0, 2);  // newest first
    ASSERT_EQ(alices.size(), 2u);
    EXPECT_EQ(alices[0].height, 111);
    EXPECT_EQ(alices[1].height, 111);
    EXPECT_NE(alices[0].coinbase, alices[1].coinbase);  // the change and the coinbase, in txid order
    expect_matches_a_rebuild();
}

// An output made and spent in the same block leaves no coin, but both transactions are in the
// address's history.
TEST_F(ChainIndexTest, HandlesSpendingWithinABlock) {
    const Transaction payment = pay(1);
    const Transaction next = onward(payment);
    mine({payment, next});
    EXPECT_EQ(index->summary(bob.public_key()), (AddressSummary{0, 2 * kCoin, 2 * kCoin, 2, 0}));
    EXPECT_EQ(index->summary(carol.public_key()), (AddressSummary{2 * kCoin - 1000, 2 * kCoin - 1000, 0, 1, 1}));
    EXPECT_TRUE(index->coins(bob.public_key(), 0, 10).empty());
    EXPECT_EQ(index->spender({payment.txid(), 0}), next.txid());
    const std::vector<AddressDelta> history = index->history(bob.public_key(), 0, 10);
    ASSERT_EQ(history.size(), 2u);
    EXPECT_EQ(history[0], (AddressDelta{next.txid(), 111, 2, 0, 2 * kCoin}));
    EXPECT_EQ(history[1], (AddressDelta{payment.txid(), 111, 1, 2 * kCoin, 0}));
    expect_matches_a_rebuild();
}

TEST_F(ChainIndexTest, AReorganisationIsUndoneExactly) {
    const BlockIndex& fork = chain.tip();
    const Transaction payment = pay(1);
    const Transaction next = onward(payment);
    mine({payment, next});
    mine({pay(2)});
    ASSERT_EQ(chain.height(), 112);

    // A rival branch of three blocks from #110 that spends block 1's coinbase differently.
    const Transaction rival = spend({coinbase_at(chain, 1)}, {{4 * kCoin, carol.public_key()}}, alice);
    const BlockIndex* tip = &fork;
    for (int i = 0; i < 3; ++i) {
        const auto time = static_cast<std::uint32_t>(kTestGenesisTime + 60 * (111 + i) + 7);
        const Block block = block_on(*tip, params, carol.public_key(), time, i == 0 ? std::vector<Transaction>{rival} : std::vector<Transaction>{});
        ASSERT_TRUE(chain.accept_block(block, kTestNow).accepted());
        tip = chain.find(block.hash());
    }
    ASSERT_EQ(chain.tip().hash, tip->hash);
    ASSERT_EQ(chain.height(), 113);

    EXPECT_EQ(index->best_block(), chain.tip().hash);
    EXPECT_EQ(index->find_tx(payment.txid()), std::nullopt);
    EXPECT_EQ(index->find_tx(next.txid()), std::nullopt);
    EXPECT_EQ(index->summary(bob.public_key()), AddressSummary{});
    EXPECT_EQ(index->spender(coinbase_at(chain, 1)), rival.txid());
    EXPECT_EQ(index->spender(coinbase_at(chain, 2)), std::nullopt);  // that payment is gone too
    EXPECT_EQ(index->summary(carol.public_key()), (AddressSummary{4 * kCoin + 15 * kCoin, 4 * kCoin + 15 * kCoin, 0, 4, 4}));
    expect_matches_a_rebuild();
}

// The node ran without --index for a while: blocks came and a reorganisation went by. Opening
// the index again backs out of the blocks that are no longer active and reads the new ones.
TEST_F(ChainIndexTest, CatchesUpAfterBeingAway) {
    const BlockIndex& fork = chain.tip();
    mine({pay(1)});
    mine({pay(2)});
    const uint256 seen = index->best_block();
    index.reset();

    const BlockIndex* tip = &fork;
    for (int i = 0; i < 4; ++i) {
        const auto time = static_cast<std::uint32_t>(kTestGenesisTime + 60 * (111 + i) + 7);
        const Block block = block_on(*tip, params, carol.public_key(), time);
        ASSERT_TRUE(chain.accept_block(block, kTestNow).accepted());
        tip = chain.find(block.hash());
    }
    ASSERT_FALSE(chain.in_active_chain(*chain.find(seen)));

    std::vector<std::string> log;
    index = std::make_unique<ChainIndex>(db, chain, [&](const std::string& line) { log.push_back(line); });
    EXPECT_EQ(index->best_block(), chain.tip().hash);
    EXPECT_EQ(index->summary(bob.public_key()), AddressSummary{});
    EXPECT_EQ(index->summary(carol.public_key()).coins, 4u);
    EXPECT_FALSE(log.empty());
    expect_matches_a_rebuild();

    // And it keeps following from there.
    mine({pay(3)});
    EXPECT_EQ(index->summary(bob.public_key()).balance, 2 * kCoin);
    expect_matches_a_rebuild();
}

TEST_F(ChainIndexTest, StartsOverWhenTheChainWasReplaced) {
    mine({pay(1)});
    index.reset();
    Chainstate another(params);  // the same network, none of the same blocks
    mine_blocks(another, carol.public_key(), 5);
    const ChainIndex reopened(db, another);
    EXPECT_EQ(reopened.best_block(), another.tip().hash);
    EXPECT_EQ(reopened.summary(alice.public_key()), AddressSummary{});
    EXPECT_EQ(reopened.summary(carol.public_key()).coins, 5u);
    MemoryStore fresh;
    const ChainIndex rebuilt(fresh, another);
    EXPECT_EQ(dump(db), dump(fresh));
}

TEST_F(ChainIndexTest, RefusesAnotherNetworksIndex) {
    index.reset();
    ConsensusParams other = easy_params();
    other.genesis_coinbase = make_coinbase(0, 5 * kCoin, PublicKey{}, bytes_of("another network"));
    other.genesis.merkle_root = genesis_block(other).compute_merkle_root();
    other.genesis = bound::mine(other.genesis, other).value();
    other.genesis_hash = other.genesis.hash();
    Chainstate foreign(other);
    EXPECT_THROW(ChainIndex(db, foreign), std::runtime_error);
}
