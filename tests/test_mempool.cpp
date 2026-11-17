#include "core/mempool.h"
#include "core/miner.h"
#include "core/validation.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace bound;

TEST(FeeRate, RoundsTheRightWay) {
    EXPECT_EQ(FeeRate::of(191, 191).per_kb, 1000);
    EXPECT_EQ(FeeRate::of(100, 3).per_kb, 33333);  // down
    EXPECT_EQ(FeeRate::of(5, 0).per_kb, 0);
    EXPECT_EQ(FeeRate{1000}.fee(191), 191);
    EXPECT_EQ(FeeRate{1500}.fee(191), 287);  // 286.5, up: paying fee(n) always meets the rate
    EXPECT_EQ(FeeRate{1}.fee(1), 1);
    EXPECT_EQ(FeeRate{0}.fee(1000), 0);
}

TEST(FeeRate, ComparesExactlyWithoutOverflow) {
    EXPECT_EQ(compare_rates(1, 3, 1, 3), std::strong_ordering::equal);
    EXPECT_EQ(compare_rates(2, 6, 1, 3), std::strong_ordering::equal);
    EXPECT_EQ(compare_rates(334, 1000, 1, 3), std::strong_ordering::greater);
    EXPECT_EQ(compare_rates(333, 1000, 1, 3), std::strong_ordering::less);
    // Products far past 2^63: 21M BOUND over 100 kB against one unit less.
    EXPECT_EQ(compare_rates(kMaxMoney, 100'000, kMaxMoney - 1, 100'000), std::strong_ordering::greater);
    EXPECT_EQ(compare_rates(kMaxMoney, 99'999, kMaxMoney, 100'000), std::strong_ordering::greater);
}

TEST(FeeRate, TransactionSizeMatchesSerialization) {
    const KeyPair key = test_key("alice");
    for (std::size_t inputs : {1u, 2u, 300u}) {
        for (std::size_t outputs : {1u, 2u, 260u}) {
            for (std::size_t memo : {0u, 100u}) {
                Transaction tx;
                tx.inputs.assign(inputs, {test_hash("x"), 0});
                tx.outputs.assign(outputs, {1, key.public_key()});
                tx.memo.assign(memo, 'm');
                sign_all_inputs(tx, key);
                EXPECT_EQ(transaction_size(inputs, outputs, memo), tx.serialize().size()) << inputs << " " << outputs << " " << memo;
            }
        }
    }
    EXPECT_EQ(transaction_size(1, 2, 0), 191u);
}

namespace {

// A chain of 110 blocks paying Alice: the coinbases of blocks 1 to 11 are spendable in block 111.
class MempoolTest : public ::testing::Test {
protected:
    MempoolTest() { mine_blocks(chain, alice.public_key(), 110); }

    // Alice's coinbase of block `height` (5 BOUND), less `fee`, to `to`.
    Transaction pay(int height, Amount fee, const PublicKey& to) const {
        return spend({coinbase_at(chain, height)}, {{5 * kCoin - fee, to}}, alice);
    }
    // Spends output `index` of `parent` (worth `value`, owned by `owner`) less `fee`, to Carol.
    Transaction child_of(const Transaction& parent, std::uint32_t index, Amount value, const KeyPair& owner, Amount fee) const {
        return spend({{parent.txid(), index}}, {{value - fee, carol.public_key()}}, owner);
    }
    MempoolAccept accept(const Transaction& tx, std::int64_t now = kTestNow) { return mempool.accept(tx, now); }

    const ConsensusParams params = easy_params();
    const KeyPair alice = test_key("alice");
    const KeyPair bob = test_key("bob");
    const KeyPair carol = test_key("carol");
    Chainstate chain{params};
    Mempool mempool{chain};
};

}  // namespace

TEST_F(MempoolTest, AcceptsAValidPayment) {
    const Transaction tx = pay(1, 1000, bob.public_key());
    const MempoolAccept r = accept(tx);
    ASSERT_TRUE(r.accepted()) << r.reason;
    EXPECT_EQ(r.fee, 1000);
    EXPECT_TRUE(r.replaced.empty());
    const MempoolEntry* e = mempool.find(tx.txid());
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->size, tx.serialize().size());
    EXPECT_EQ(e->fee, 1000);
    EXPECT_EQ(e->time, kTestNow);
    EXPECT_EQ(mempool.find_wtxid(tx.wtxid()), e);
    EXPECT_EQ(mempool.spender(coinbase_at(chain, 1)), e);
    EXPECT_EQ(mempool.size(), 1u);
    EXPECT_EQ(mempool.bytes(), e->size);
    EXPECT_EQ(mempool.total_fee(), 1000);
    EXPECT_EQ(accept(tx).result, MempoolResult::Duplicate);
}

TEST_F(MempoolTest, RefusesWhatBreaksConsensus) {
    Transaction coinbase = chain.block_at(5).txs[0];
    EXPECT_EQ(accept(coinbase).result, MempoolResult::Invalid);

    Transaction forged = pay(1, 1000, bob.public_key());
    forged.signatures[0][5] ^= 0x40;
    EXPECT_EQ(accept(forged).result, MempoolResult::Invalid);

    EXPECT_EQ(accept(pay(1, -1, bob.public_key())).result, MempoolResult::Invalid);  // pays out 1 unit more than it has

    Transaction no_outputs = spend({coinbase_at(chain, 1)}, {}, alice);
    EXPECT_EQ(accept(no_outputs).result, MempoolResult::Invalid);
    EXPECT_EQ(mempool.size(), 0u);
}

TEST_F(MempoolTest, RefusesNonStandardTransactions) {
    Transaction v2 = pay(1, 1000, bob.public_key());
    v2.version = 2;
    sign_all_inputs(v2, alice);
    EXPECT_EQ(accept(v2).result, MempoolResult::NonStandard);

    const Transaction dust = spend({coinbase_at(chain, 1)}, {{419, bob.public_key()}, {5 * kCoin - 419 - 1000, alice.public_key()}}, alice);
    EXPECT_EQ(accept(dust).result, MempoolResult::NonStandard);
    const Transaction just_enough = spend({coinbase_at(chain, 1)}, {{420, bob.public_key()}, {5 * kCoin - 420 - 1000, alice.public_key()}}, alice);
    EXPECT_TRUE(accept(just_enough).accepted());

    MempoolOptions small;
    small.max_tx_size = 150;  // a 1-in, 1-out payment is 151 bytes
    Mempool strict(chain, small);
    EXPECT_EQ(strict.accept(pay(2, 1000, bob.public_key()), kTestNow).result, MempoolResult::NonStandard);
}

TEST_F(MempoolTest, TheMinimumFeeIsOneUnitPerByte) {
    const Transaction tx = pay(1, 151, bob.public_key());
    ASSERT_EQ(tx.serialize().size(), 151u);
    EXPECT_EQ(accept(pay(1, 150, bob.public_key())).result, MempoolResult::FeeTooLow);
    EXPECT_EQ(accept(pay(1, 0, bob.public_key())).result, MempoolResult::FeeTooLow);
    EXPECT_TRUE(accept(tx).accepted());
}

TEST_F(MempoolTest, MissingAndImmatureInputs) {
    const Transaction unknown = spend({{test_hash("nothing"), 0}}, {{1000, bob.public_key()}}, alice);
    EXPECT_EQ(accept(unknown).result, MempoolResult::MissingInputs);
    const Transaction bad_index = spend({{coinbase_at(chain, 1).txid, 1}}, {{1000, bob.public_key()}}, alice);
    EXPECT_EQ(accept(bad_index).result, MempoolResult::MissingInputs);
    EXPECT_EQ(accept(pay(12, 1000, bob.public_key())).result, MempoolResult::Premature);  // block 12's coinbase: 99 deep
    EXPECT_TRUE(accept(pay(11, 1000, bob.public_key())).accepted());                       // block 11's: 100
}

TEST_F(MempoolTest, UnconfirmedChainsAreLinked) {
    const Transaction parent = pay(1, 1000, bob.public_key());
    const Transaction child = child_of(parent, 0, 5 * kCoin - 1000, bob, 1000);
    EXPECT_EQ(accept(child).result, MempoolResult::MissingInputs);  // the parent comes first
    ASSERT_TRUE(accept(parent).accepted());
    ASSERT_TRUE(accept(child).accepted());
    EXPECT_EQ(mempool.find(child.txid())->parents, std::set<uint256>{parent.txid()});
    EXPECT_EQ(mempool.find(parent.txid())->children, std::set<uint256>{child.txid()});
    const std::vector<const MempoolEntry*> family = mempool.with_descendants(parent.txid());
    ASSERT_EQ(family.size(), 2u);
    EXPECT_EQ(family[0]->txid, parent.txid());
    EXPECT_EQ(family[1]->txid, child.txid());
}

TEST_F(MempoolTest, LongUnconfirmedChainsAreRefused) {
    MempoolOptions options;
    options.ancestor_limit = 3;
    options.descendant_limit = 25;
    Mempool short_chains(chain, options);
    Transaction tx = pay(1, 1000, alice.public_key());
    Amount value = 5 * kCoin - 1000;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(short_chains.accept(tx, kTestNow).accepted()) << i;
        tx = spend({{tx.txid(), 0}}, {{value - 1000, alice.public_key()}}, alice);
        value -= 1000;
    }
    EXPECT_EQ(short_chains.accept(tx, kTestNow).result, MempoolResult::TooLongChain);  // a 4th generation
}

TEST_F(MempoolTest, TooManyDescendantsAreRefused) {
    MempoolOptions options;
    options.descendant_limit = 3;
    Mempool few(chain, options);
    std::vector<TxOut> outs(4, TxOut{kCoin, bob.public_key()});
    const Transaction fan = spend({coinbase_at(chain, 1)}, outs, alice);  // 4 BOUND out of 5: 1 BOUND fee
    ASSERT_TRUE(few.accept(fan, kTestNow).accepted());
    ASSERT_TRUE(few.accept(child_of(fan, 0, kCoin, bob, 1000), kTestNow).accepted());
    ASSERT_TRUE(few.accept(child_of(fan, 1, kCoin, bob, 1000), kTestNow).accepted());
    EXPECT_EQ(few.accept(child_of(fan, 2, kCoin, bob, 1000), kTestNow).result, MempoolResult::TooLongChain);
}

// Full replace-by-fee: a double spend gets in by paying a higher rate than what it conflicts with,
// and more in total than everything it evicts, plus its own relay.
TEST_F(MempoolTest, ReplaceByFee) {
    const Bytes memo(100, 'x');
    const Transaction original = spend({coinbase_at(chain, 1)}, {{5 * kCoin - 2510, bob.public_key()}}, alice, memo);
    ASSERT_EQ(original.serialize().size(), 251u);  // 10 units per byte
    ASSERT_TRUE(accept(original).accepted());

    EXPECT_EQ(accept(pay(1, 1510, carol.public_key())).result, MempoolResult::Conflict);  // 151 bytes at exactly 10/byte
    // A better rate (2660 / 151 = 17.6 per byte), but 1 unit short of 2510 + 151:
    EXPECT_EQ(accept(pay(1, 2660, carol.public_key())).result, MempoolResult::Conflict);
    const Transaction replacement = pay(1, 2661, carol.public_key());
    const MempoolAccept r = accept(replacement);
    ASSERT_TRUE(r.accepted()) << r.reason;
    EXPECT_EQ(r.replaced, std::vector<uint256>{original.txid()});
    EXPECT_FALSE(mempool.contains(original.txid()));
    EXPECT_EQ(mempool.spender(coinbase_at(chain, 1))->txid, replacement.txid());
    EXPECT_EQ(mempool.size(), 1u);
}

TEST_F(MempoolTest, AReplacementPaysForTheDescendantsItEvicts) {
    const Transaction parent = pay(1, 1000, bob.public_key());
    const Transaction child = child_of(parent, 0, 5 * kCoin - 1000, bob, 50'000);
    ASSERT_TRUE(accept(parent).accepted());
    ASSERT_TRUE(accept(child).accepted());

    EXPECT_EQ(accept(pay(1, 50'000, carol.public_key())).result, MempoolResult::Conflict);  // beats the parent alone
    const MempoolAccept r = accept(pay(1, 51'151, carol.public_key()));
    ASSERT_TRUE(r.accepted()) << r.reason;
    EXPECT_EQ(r.replaced.size(), 2u);
    EXPECT_EQ(mempool.size(), 1u);

    // Nor may a replacement spend what it replaces.
    const Transaction mine = pay(2, 1000, alice.public_key());
    ASSERT_TRUE(accept(mine).accepted());
    const Transaction sneaky = spend({coinbase_at(chain, 2), {mine.txid(), 0}}, {{kCoin, carol.public_key()}}, alice);
    EXPECT_EQ(accept(sneaky).result, MempoolResult::Conflict);
}

TEST_F(MempoolTest, ABlockTakesItsTransactionsAndTheirConflictsOut) {
    const Transaction a = pay(1, 1000, bob.public_key());
    const Transaction a_child = child_of(a, 0, 5 * kCoin - 1000, bob, 1000);
    const Transaction b = pay(2, 1000, bob.public_key());
    const Transaction b_child = child_of(b, 0, 5 * kCoin - 1000, bob, 1000);
    for (const Transaction& tx : {a, a_child, b, b_child}) ASSERT_TRUE(accept(tx).accepted());

    // The block confirms a, and double-spends b's coin elsewhere.
    const Transaction b_elsewhere = pay(2, 5000, carol.public_key());
    ASSERT_TRUE(chain.accept_block(next_block(chain, alice.public_key(), {a, b_elsewhere}), kTestNow).accepted());

    EXPECT_FALSE(mempool.contains(a.txid()));
    EXPECT_TRUE(mempool.contains(a_child.txid()));  // its parent is confirmed now
    EXPECT_TRUE(mempool.find(a_child.txid())->parents.empty());
    EXPECT_FALSE(mempool.contains(b.txid()));
    EXPECT_FALSE(mempool.contains(b_child.txid()));
    EXPECT_EQ(mempool.size(), 1u);
}

// A reorganisation puts the losing blocks' transactions back, unless the new chain has them or
// double-spends them.
TEST_F(MempoolTest, AReorganisationPutsTransactionsBack) {
    const Transaction kept = pay(1, 1000, bob.public_key());
    const Transaction lost = pay(2, 1000, bob.public_key());
    const Transaction waiting = child_of(kept, 0, 5 * kCoin - 1000, bob, 1000);
    const BlockIndex& fork = chain.tip();
    ASSERT_TRUE(chain.accept_block(next_block(chain, alice.public_key(), {kept, lost}), kTestNow).accepted());
    ASSERT_TRUE(accept(waiting).accepted());  // spends `kept`, confirmed in #111

    // A rival branch from #110, two blocks long: its #111 double-spends `lost`'s coin.
    const Transaction rival = pay(2, 7000, carol.public_key());
    const Block r1 = block_on(fork, params, carol.public_key(), kTestGenesisTime + 60 * 111 + 1, {rival});
    ASSERT_TRUE(chain.accept_block(r1, kTestNow).accepted());
    const Block r2 = block_on(*chain.find(r1.hash()), params, carol.public_key(), kTestGenesisTime + 60 * 112 + 1);
    ASSERT_TRUE(chain.accept_block(r2, kTestNow).accepted());
    ASSERT_EQ(chain.tip().hash, r2.hash());

    EXPECT_TRUE(mempool.contains(kept.txid()));     // back
    EXPECT_TRUE(mempool.contains(waiting.txid()));  // still, now spending an unconfirmed parent again
    EXPECT_FALSE(mempool.contains(lost.txid()));    // the new chain spent its coin
    EXPECT_EQ(mempool.find(waiting.txid())->parents, std::set<uint256>{kept.txid()});
}

TEST_F(MempoolTest, OldTransactionsExpire) {
    const Transaction old = pay(1, 1000, bob.public_key());
    const Transaction old_child = child_of(old, 0, 5 * kCoin - 1000, bob, 1000);
    const Transaction young = pay(2, 1000, bob.public_key());
    ASSERT_TRUE(accept(old, kTestNow).accepted());
    ASSERT_TRUE(accept(old_child, kTestNow + 1000).accepted());
    ASSERT_TRUE(accept(young, kTestNow + 1000).accepted());
    const std::int64_t two_weeks = 14 * 24 * 3600;
    EXPECT_EQ(mempool.expire(kTestNow + two_weeks), 0u);
    EXPECT_EQ(mempool.expire(kTestNow + two_weeks + 1), 2u);  // with its child, however young
    EXPECT_TRUE(mempool.contains(young.txid()));
}

TEST_F(MempoolTest, AFullMempoolEvictsTheLowestFeeRate) {
    MempoolOptions options;
    options.max_bytes = 3 * 151;
    Mempool small(chain, options);
    const Transaction cheap = pay(1, 200, bob.public_key());
    const Transaction mid = pay(2, 400, bob.public_key());
    const Transaction high = pay(3, 600, bob.public_key());
    for (const Transaction& tx : {cheap, mid, high}) ASSERT_TRUE(small.accept(tx, kTestNow).accepted());

    EXPECT_EQ(small.accept(pay(4, 151, bob.public_key()), kTestNow).result, MempoolResult::MempoolFull);
    EXPECT_EQ(small.size(), 3u);
    ASSERT_TRUE(small.accept(pay(5, 300, bob.public_key()), kTestNow).accepted());
    EXPECT_FALSE(small.contains(cheap.txid()));
    EXPECT_EQ(small.size(), 3u);
    EXPECT_LE(small.bytes(), options.max_bytes);
}

// A child paying a high fee drags its cheap parent in ahead of a middling transaction.
TEST_F(MempoolTest, SelectionLetsChildrenPayForParents) {
    const Transaction parent = pay(1, 151, bob.public_key());                       // 1 per byte
    const Transaction child = child_of(parent, 0, 5 * kCoin - 151, bob, 151 * 40);  // 40 per byte
    const Transaction middling = pay(2, 151 * 10, bob.public_key());               // 10 per byte
    for (const Transaction& tx : {parent, child, middling}) ASSERT_TRUE(accept(tx).accepted());

    const Mempool::Selection all = mempool.select(1'000'000);
    ASSERT_EQ(all.txs.size(), 3u);
    EXPECT_EQ(all.txs[0].txid(), parent.txid());  // the package (20.5 per byte) first, parent first
    EXPECT_EQ(all.txs[1].txid(), child.txid());
    EXPECT_EQ(all.txs[2].txid(), middling.txid());
    EXPECT_TRUE(all.all);
    EXPECT_EQ(all.fees, 151 + 151 * 40 + 151 * 10);
    EXPECT_EQ(all.lowest, FeeRate{10000});

    const Mempool::Selection two = mempool.select(2 * 151);
    EXPECT_EQ(two.txs.size(), 2u);
    EXPECT_FALSE(two.all);
    EXPECT_EQ(mempool.next_block_fee_rate(2 * 151), FeeRate{20500 + 1000});
    EXPECT_EQ(mempool.next_block_fee_rate(1'000'000), mempool.options().min_relay_fee);

    // What's selected always makes a valid block.
    EXPECT_TRUE(chain.accept_block(next_block(chain, alice.public_key(), all.txs), kTestNow).accepted());
    EXPECT_EQ(mempool.size(), 0u);
}

TEST_F(MempoolTest, ListenersHearOfNewTransactions) {
    struct Recorder : MempoolListener {
        std::vector<uint256> seen;
        void tx_added(const Transaction& tx, std::int64_t) override { seen.push_back(tx.txid()); }
    } recorder;
    mempool.add_listener(recorder);
    const Transaction tx = pay(1, 1000, bob.public_key());
    ASSERT_TRUE(accept(tx).accepted());
    EXPECT_EQ(accept(pay(2, 0, bob.public_key())).result, MempoolResult::FeeTooLow);
    mempool.remove_listener(recorder);
    EXPECT_EQ(recorder.seen, std::vector<uint256>{tx.txid()});
}
