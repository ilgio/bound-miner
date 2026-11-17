#include "core/consensus.h"
#include "core/params.h"
#include "core/pow.h"
#include "core/validation.h"
#include "test_util.h"
#include "util/hex.h"

#include <string>

#include <gtest/gtest.h>

using namespace bound;

TEST(Params, PowLimitRoundTripsThroughCompact) {
    const ConsensusParams& p = testnet_params();
    EXPECT_EQ(target_from_compact(p.pow_limit_bits), p.pow_limit);
    EXPECT_EQ(compact_from_target(p.pow_limit), p.pow_limit_bits);
}

TEST(Params, GenesisIsValid) {
    const ConsensusParams& p = testnet_params();
    EXPECT_TRUE(p.genesis.prev_hash.is_zero());
    EXPECT_EQ(p.genesis.bits, p.pow_limit_bits);
    EXPECT_EQ(p.genesis.hash(), p.genesis_hash);
    EXPECT_TRUE(check_proof_of_work(p.genesis, p));
}

TEST(Params, TimeRulesAreSet) {
    const ConsensusParams& p = testnet_params();
    EXPECT_GT(p.max_future_block_time, 0);
    EXPECT_GT(p.median_time_span, 0);
}

// Milestone 2: the economics you chose, and a genesis block with a real coinbase.
TEST(Params, Economics) {
    const ConsensusParams& p = testnet_params();
    EXPECT_EQ(p.target_spacing, 60);
    EXPECT_EQ(p.initial_subsidy, 5 * kCoin);
    EXPECT_EQ(p.halving_interval, 2'100'000);  // about 4 years of 1-minute blocks
    EXPECT_EQ(p.max_block_size, 1'000'000u);
    EXPECT_EQ(p.address_version, 0xf0);       // testnet addresses start with '9'
}

TEST(Params, GenesisBlockIsValid) {
    const ConsensusParams& p = testnet_params();
    const Transaction& cb = p.genesis_coinbase;
    EXPECT_TRUE(cb.is_coinbase());
    EXPECT_EQ(cb.lock_time, 0u);  // height 0
    ASSERT_EQ(cb.outputs.size(), 1u);
    EXPECT_EQ(cb.outputs[0].amount, block_subsidy(0, p));

    const Block g = genesis_block(p);
    EXPECT_EQ(g.compute_merkle_root(), p.genesis.merkle_root);
    EXPECT_EQ(check_block(g, p), BlockCheck::Ok);
}

// Milestone 3: your choices of 100-block maturity and an unspendable genesis reward.
TEST(Params, CoinbaseMaturity) {
    EXPECT_EQ(testnet_params().coinbase_maturity, 100);
}

TEST(Params, GenesisRewardIsUnspendable) {
    const ConsensusParams& p = testnet_params();
    EXPECT_EQ(to_hex(p.genesis_coinbase.outputs[0].recipient), std::string(64, '0'));  // a key nobody holds
    const Chainstate chain(p);
    EXPECT_EQ(chain.utxos().size(), 0u);  // and it never enters the UTXO set
}

// Milestone 4: your choices of Bitcoin's retarget rule over a 1-day window, and Bitcoin's 2-hour
// limit on future timestamps.
TEST(Params, DifficultyRetargetsDaily) {
    const ConsensusParams& p = testnet_params();
    EXPECT_EQ(p.retarget_interval, 1440);
    EXPECT_EQ(p.retarget_interval * p.target_spacing, 24 * 60 * 60);
    EXPECT_EQ(p.max_future_block_time, 2 * 60 * 60);
}
