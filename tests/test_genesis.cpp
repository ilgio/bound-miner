// Milestone 15: making a genesis block (core/genesis.h), as launch day does for mainnet.
#include "core/consensus.h"
#include "core/genesis.h"
#include "core/params.h"
#include "core/pow.h"
#include "core/validation.h"

#include <gtest/gtest.h>

#include <stop_token>
#include <string>

using namespace bound;

namespace {

std::string memo_of(const ConsensusParams& p) { return {p.genesis_coinbase.memo.begin(), p.genesis_coinbase.memo.end()}; }

// Testnet's rules with regtest's small BoundHash and a target `bits`, so a search takes a few
// thousand quick hashes even in Debug. (The real genesis blocks are checked in test_networks.cpp,
// and `boundd --testnet genesis` mines the testnet's again in a few seconds.)
ConsensusParams quick(std::uint32_t bits) {
    ConsensusParams p = testnet_params();
    p.pow_lines = regtest_params().pow_lines;
    p.pow_limit_bits = bits;
    p.pow_limit = target_from_compact(bits).value();
    return p;
}

}  // namespace

// The coinbase is Bitcoin's kind: no inputs, the reward to a key nobody holds, the message as memo.
TEST(Genesis, MakesBitcoinsKindOfCoinbase) {
    const std::optional<Block> g = make_genesis("BOUND 01/Jan/2027 a headline", 50 * kCoin, 1798761600, 0x1d00ffff);
    ASSERT_TRUE(g.has_value());
    ASSERT_EQ(g->txs.size(), 1u);
    const Transaction& coinbase = g->txs[0];
    EXPECT_TRUE(coinbase.is_coinbase());
    ASSERT_EQ(coinbase.outputs.size(), 1u);
    EXPECT_EQ(coinbase.outputs[0].amount, 50 * kCoin);
    EXPECT_EQ(coinbase.outputs[0].recipient, PublicKey{});
    EXPECT_EQ(std::string(coinbase.memo.begin(), coinbase.memo.end()), "BOUND 01/Jan/2027 a headline");
    EXPECT_EQ(g->header.version, 1);
    EXPECT_TRUE(g->header.prev_hash.is_zero());
    EXPECT_EQ(g->header.merkle_root, g->compute_merkle_root());
    EXPECT_EQ(g->header.time, 1798761600u);
    EXPECT_EQ(g->header.bits, 0x1d00ffffu);

    EXPECT_TRUE(make_genesis(std::string(Transaction::kMaxMemo, 'x'), 50 * kCoin, 0, 0x1d00ffff).has_value());
    EXPECT_FALSE(make_genesis(std::string(Transaction::kMaxMemo + 1, 'x'), 50 * kCoin, 0, 0x1d00ffff).has_value());
}

// Mined from the testnet's message and time, a genesis comes out the same on one thread or eight,
// and it's the first solution: the search always ends there, so anyone can check a genesis.
TEST(Genesis, FindsTheSameBlockOnAnyNumberOfThreads) {
    const ConsensusParams p = quick(0x1f0fffff);  // about 1 hash in 4,096
    const std::optional<Block> g = make_genesis(memo_of(p), block_subsidy(0, p), p.genesis.time, p.pow_limit_bits);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->header.merkle_root, testnet_params().genesis.merkle_root);
    const std::optional<BlockHeader> first = mine_genesis(g->header, p, 1);
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(check_proof_of_work(*first, p));
    EXPECT_EQ(first->time, p.genesis.time);
    for (std::uint32_t n = 0; n < first->nonce; ++n) {
        BlockHeader h = first.value();
        h.nonce = n;
        ASSERT_FALSE(check_proof_of_work(h, p)) << n;
    }
    const std::optional<BlockHeader> eight = mine_genesis(g->header, p, 8);
    ASSERT_TRUE(eight.has_value());
    EXPECT_EQ(eight->hash(), first->hash());
}

// When no nonce works at the start time, the search moves on a second, and keeps the first answer.
TEST(Genesis, MovesTheTimeOnWhenNoNonceWorks) {
    const ConsensusParams p = quick(0x1f0fffff);  // about 1 hash in 4,096
    const std::optional<Block> g = make_genesis("a genesis that needs more than one second", 5 * kCoin, 1'800'000'000, p.pow_limit_bits);
    ASSERT_TRUE(g.has_value());
    // 100 nonces a second: the first second fails with 97.6% odds, so this finds one later.
    constexpr std::uint64_t kPerSecond = 100;
    const std::optional<BlockHeader> mined = mine_genesis(g->header, p, 4, {}, {}, kPerSecond);
    ASSERT_TRUE(mined.has_value());
    EXPECT_TRUE(check_proof_of_work(*mined, p));
    EXPECT_LT(mined->nonce, kPerSecond);
    EXPECT_GE(mined->time, g->header.time);
    // The first answer in (time, nonce) order: nothing earlier in that second, nor in any second before.
    for (std::uint32_t t = g->header.time; t <= mined->time; ++t) {
        BlockHeader h = g->header;
        h.time = t;
        for (std::uint32_t n = 0; n < (t == mined->time ? mined->nonce : kPerSecond); ++n) {
            h.nonce = n;
            ASSERT_FALSE(check_proof_of_work(h, p)) << t << " " << n;
        }
    }
    // And the same on one thread.
    const std::optional<BlockHeader> again = mine_genesis(g->header, p, 1, {}, {}, kPerSecond);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->hash(), mined->hash());
}

TEST(Genesis, RefusesBitsEasierThanTheLimitAndStopsWhenAsked) {
    const ConsensusParams& p = mainnet_params();
    const std::optional<Block> g = make_genesis("x", 50 * kCoin, 1'800'000'000, 0x207fffff);  // regtest's: far too easy
    ASSERT_TRUE(g.has_value());
    EXPECT_FALSE(mine_genesis(g->header, p, 2).has_value());

    std::stop_source stop;
    stop.request_stop();
    BlockHeader hard = g->header;
    hard.bits = p.pow_limit_bits;
    EXPECT_FALSE(mine_genesis(hard, p, 2, stop.get_token()).has_value());
}
