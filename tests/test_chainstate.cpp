#include "core/consensus.h"
#include "core/miner.h"
#include "core/validation.h"
#include "core/wallet.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

using namespace bound;

namespace {

class ChainstateTest : public ::testing::Test {
protected:
    // A mined block on the tip *without* the template's validation, so tests can build invalid ones.
    Block forced_block(std::vector<Transaction> txs, Amount claim) const {
        Block block;
        block.txs.push_back(make_coinbase(chain.height() + 1, claim, alice.public_key(), {}));
        for (Transaction& tx : txs) block.txs.push_back(std::move(tx));
        block.header.prev_hash = chain.tip().hash;
        block.header.merkle_root = block.compute_merkle_root();
        block.header.time = static_cast<std::uint32_t>(kTestGenesisTime + 60 * (chain.height() + 1));
        block.header.bits = next_work_required(chain.tip(), params);
        block.header = mine(block.header, params).value();
        return block;
    }

    // Alice spends the coinbase of the block at `height` to Bob, keeping `fee` back.
    Transaction spend_coinbase_of(int height, Amount fee) const {
        Transaction tx;
        tx.inputs = {{chain.block_at(height).txs[0].txid(), 0}};
        tx.outputs = {{5 * kCoin - fee, bob.public_key()}};
        sign_all_inputs(tx, alice);
        return tx;
    }

    const ConsensusParams params = easy_params();
    Chainstate chain{params};
    const KeyPair alice = test_key("alice");
    const KeyPair bob = test_key("bob");
};

}  // namespace

TEST_F(ChainstateTest, StartsAtGenesisWithNothingSpendable) {
    EXPECT_EQ(chain.height(), 0);
    EXPECT_EQ(chain.block_at(0), genesis_block(params));
    EXPECT_EQ(chain.utxos().size(), 0u);  // the genesis reward never enters the UTXO set
}

TEST_F(ChainstateTest, MiningPaysTheMiner) {
    mine_blocks(chain, alice.public_key(), 3);
    EXPECT_EQ(chain.height(), 3);
    EXPECT_EQ(chain.utxos().size(), 3u);
    EXPECT_EQ(balance(chain.utxos(), alice.public_key()), 15 * kCoin);
    const Coin* coin = chain.utxos().find({chain.block_at(2).txs[0].txid(), 0});
    ASSERT_NE(coin, nullptr);
    EXPECT_TRUE(coin->coinbase);
    EXPECT_EQ(coin->height, 2);
}

TEST_F(ChainstateTest, SupplyIsSubsidyTimesHeight) {
    mine_blocks(chain, alice.public_key(), 25);
    EXPECT_EQ(chain.utxos().total_value(), 25 * 5 * kCoin);
}

// The coinbase from block 1 may first be spent in block 101 (100 blocks deep).
TEST_F(ChainstateTest, CoinbaseMaturity) {
    mine_blocks(chain, alice.public_key(), 99);
    EXPECT_TRUE(spendable_coins(chain.utxos(), alice.public_key(), 100, params).empty());

    const BlockAcceptResult early = chain.accept_block(forced_block({spend_coinbase_of(1, kCoin)}, 6 * kCoin), kTestNow);
    EXPECT_EQ(early.header, AcceptResult::Accepted);
    EXPECT_EQ(early.body, ConnectResult::BadInputs);  // block 100 would be only 99 deep
    EXPECT_EQ(chain.height(), 99);

    mine_blocks(chain, alice.public_key(), 1);
    const BlockAcceptResult on_time = chain.accept_block(next_block(chain, alice.public_key(), {spend_coinbase_of(1, kCoin)}), kTestNow);
    EXPECT_TRUE(on_time.accepted());
    EXPECT_EQ(balance(chain.utxos(), bob.public_key()), 4 * kCoin);
}

TEST_F(ChainstateTest, PaymentEndToEnd) {
    mine_blocks(chain, alice.public_key(), 101);
    const Amount fee = kCoin / 1000;
    const std::optional<Transaction> pay =
        create_payment(chain.utxos(), alice, bob.public_key(), 7 * kCoin + kCoin / 2, fee, 102, params);
    ASSERT_TRUE(pay.has_value());
    ASSERT_TRUE(chain.accept_block(next_block(chain, alice.public_key(), {*pay}), kTestNow).accepted());

    EXPECT_EQ(chain.height(), 102);
    EXPECT_EQ(balance(chain.utxos(), bob.public_key()), 7 * kCoin + kCoin / 2);
    EXPECT_EQ(chain.utxos().total_value(), 102 * 5 * kCoin);  // the fee moved to the miner; nothing was created
    EXPECT_EQ(balance(chain.utxos(), alice.public_key()), 102 * 5 * kCoin - (7 * kCoin + kCoin / 2));
}

TEST_F(ChainstateTest, GenesisRewardCantBeSpent) {
    mine_blocks(chain, alice.public_key(), 101);
    Transaction steal;
    steal.inputs = {{params.genesis_coinbase.txid(), 0}};
    steal.outputs = {{5 * kCoin, alice.public_key()}};
    sign_all_inputs(steal, alice);
    EXPECT_EQ(chain.accept_block(forced_block({steal}, 5 * kCoin), kTestNow).body, ConnectResult::BadInputs);
}

TEST_F(ChainstateTest, RejectedBodyLeavesEverythingUntouched) {
    mine_blocks(chain, alice.public_key(), 2);
    const UtxoSet before = chain.utxos();
    const uint256 tip = chain.tip().hash;

    EXPECT_EQ(chain.accept_block(forced_block({}, 5 * kCoin + 1), kTestNow).body, ConnectResult::CoinbaseTooLarge);
    EXPECT_EQ(chain.height(), 2);
    EXPECT_EQ(chain.tip().hash, tip);
    EXPECT_TRUE(chain.utxos() == before);
}

TEST_F(ChainstateTest, HeaderRulesStillApply) {
    mine_blocks(chain, alice.public_key(), 1);
    Block orphan = forced_block({}, 5 * kCoin);
    orphan.header.prev_hash = test_hash("a block nobody has seen");
    orphan.header = mine(orphan.header, params).value();
    const BlockAcceptResult r = chain.accept_block(orphan, kTestNow);
    EXPECT_EQ(r.header, AcceptResult::UnknownParent);
    EXPECT_FALSE(r.accepted());
}

TEST_F(ChainstateTest, TemplateBuildsOnTheTip) {
    mine_blocks(chain, alice.public_key(), 3);
    const std::optional<Block> t = make_block_template(chain, bob.public_key(), {}, 0);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->header.prev_hash, chain.tip().hash);
    EXPECT_EQ(t->header.bits, next_work_required(chain.tip(), params));
    EXPECT_EQ(t->header.time, chain.tip().median_time_past(params.median_time_span) + 1);  // time 0 is bumped past the median
    EXPECT_EQ(t->header.merkle_root, t->compute_merkle_root());
    ASSERT_EQ(t->txs.size(), 1u);
    EXPECT_EQ(t->txs[0].lock_time, 4u);
    EXPECT_EQ(t->txs[0].outputs[0].amount, 5 * kCoin);
    EXPECT_EQ(t->txs[0].memo, Bytes(8, 0));  // the extra nonce
}

TEST_F(ChainstateTest, TemplateCollectsFees) {
    mine_blocks(chain, alice.public_key(), 101);
    const std::optional<Block> t =
        make_block_template(chain, bob.public_key(), {spend_coinbase_of(1, 3000), spend_coinbase_of(2, 4000)}, 0);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->txs[0].outputs[0].amount, 5 * kCoin + 7000);
}

TEST_F(ChainstateTest, TemplateRefusesInvalidTransactions) {
    mine_blocks(chain, alice.public_key(), 101);
    EXPECT_FALSE(make_block_template(chain, bob.public_key(), {spend_coinbase_of(2, 0), spend_coinbase_of(2, 0)}, 0));
    Transaction unsigned_tx = spend_coinbase_of(1, 0);
    unsigned_tx.signatures.clear();
    EXPECT_FALSE(make_block_template(chain, bob.public_key(), {unsigned_tx}, 0));
    EXPECT_FALSE(make_block_template(chain, bob.public_key(), {spend_coinbase_of(50, 0)}, 0));  // immature
}

TEST_F(ChainstateTest, MinedTemplatesAreValid) {
    const Block block = next_block(chain, alice.public_key());
    EXPECT_EQ(check_block(block, params), BlockCheck::Ok);
    EXPECT_TRUE(chain.accept_block(block, kTestNow).accepted());
}
