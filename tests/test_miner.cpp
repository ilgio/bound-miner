#include "core/miner.h"
#include "core/pow.h"
#include "test_util.h"
#include "util/hex.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

using namespace bound;

namespace {

// About 1 in 256 hashes meets this target, so these tests run instantly even in Debug.
constexpr std::uint32_t kEasyBits = 0x2000ffff;

// The first two valid nonces for template_header(), computed independently: BoundHash with 64
// lines, by the Python reference implementation (contrib/boundhash/boundhash.py).
constexpr std::uint32_t kFirstValid = 325;
constexpr std::uint32_t kSecondValid = 432;

// Regtest's pow limit and BoundHash size.
const ConsensusParams& pow_params() { return regtest_params(); }

BlockHeader template_header() {
    BlockHeader h;
    h.version = 1;
    h.prev_hash = uint256(sha256d(bytes_of("previous block")));
    h.merkle_root = uint256(tagged_hash("BOUND/genesis", bytes_of("miner test")));
    h.time = 1'800'000'000;
    h.bits = kEasyBits;
    return h;
}

}  // namespace

TEST(Miner, FindsTheFirstValidNonce) {
    const std::optional<BlockHeader> mined = mine(template_header(), pow_params());
    ASSERT_TRUE(mined.has_value());
    EXPECT_EQ(mined->nonce, kFirstValid);
    EXPECT_TRUE(check_proof_of_work(*mined, pow_params()));

    BlockHeader expected = template_header();
    expected.nonce = kFirstValid;
    EXPECT_EQ(*mined, expected);  // nothing but the nonce changed
}

TEST(Miner, StartsAtStartNonce) {
    const std::optional<BlockHeader> mined = mine(template_header(), pow_params(), kFirstValid + 1);
    ASSERT_TRUE(mined.has_value());
    EXPECT_EQ(mined->nonce, kSecondValid);
}

TEST(Miner, StopsAfterMaxTries) {
    EXPECT_FALSE(mine(template_header(), pow_params(), 0, kFirstValid).has_value());  // nonces 0..324
    EXPECT_TRUE(mine(template_header(), pow_params(), 0, kFirstValid + 1).has_value());
}

TEST(Miner, NeverWrapsPastMaxNonce) {
    // None of the last 10 nonces is valid. A miner that wrapped around to 0 would reach
    // kFirstValid within the extra tries allowed here.
    const std::uint32_t start = 0xffffffff - 9;
    EXPECT_FALSE(mine(template_header(), pow_params(), start, 10 + kFirstValid + 1).has_value());
}

// Milestone 2.
TEST(Coinbase, Shape) {
    const PublicKey alice = test_key("alice").public_key();
    const Transaction cb = make_coinbase(7, 5 * kCoin, alice, bytes_of("pool A"));
    EXPECT_TRUE(cb.is_coinbase());
    EXPECT_EQ(cb.version, 1);
    ASSERT_EQ(cb.outputs.size(), 1u);
    EXPECT_EQ(cb.outputs[0].amount, 5 * kCoin);
    EXPECT_EQ(to_hex(cb.outputs[0].recipient), to_hex(alice));
    EXPECT_EQ(cb.lock_time, 7u);
    EXPECT_EQ(cb.memo, (Bytes{'p', 'o', 'o', 'l', ' ', 'A'}));
    EXPECT_TRUE(cb.signatures.empty());
}

TEST(Coinbase, KnownSerializationAndTxid) {
    const Transaction cb = make_coinbase(7, 5 * kCoin, test_key("alice").public_key(), bytes_of("pool A"));
    EXPECT_EQ(to_hex(cb.serialize_body()),
              "01000000"                                                          // version
              "00"                                                                // no inputs
              "01" "0065cd1d00000000"                                             // 1 output: 5 BOUND
              "d5bf4a3fcce717b0388bcc2749ebc148ad9969b23f45ee1b605fd58778576ac4"  // to alice
              "07000000"                                                          // lock_time = height 7
              "06" "706f6f6c2041");                                               // memo "pool A"
    EXPECT_EQ(cb.txid().to_hex(), "c5713ead2fd5332931914fbc8f337fb105482d16fb9151b218f19f23dedeeb65");
    EXPECT_EQ(cb.wtxid().to_hex(), "e20f4026a7a2acb42db5b6f2d011065975f88583e5eb94d025636bfa15149f6a");
}

// Two coinbases paying the same key the same amount would otherwise share a txid, and the
// second would overwrite the first in the UTXO set (Bitcoin's BIP30 problem).
TEST(Coinbase, HeightMakesTxidsUnique) {
    const PublicKey alice = test_key("alice").public_key();
    EXPECT_NE(make_coinbase(1, 5 * kCoin, alice, {}).txid(), make_coinbase(2, 5 * kCoin, alice, {}).txid());
}

// Milestone 12: several threads, each searching its own coinbases. Each piece of work yields one
// block, and it's valid.
TEST(BackgroundMiner, SeveralThreadsFindValidBlocks) {
    using namespace std::chrono_literals;
    const ConsensusParams p = easy_params();
    Chainstate chain(p);
    BackgroundMiner miner(p, 4);
    EXPECT_EQ(miner.threads(), 4u);
    EXPECT_GE(BackgroundMiner::default_threads(), 1u);
    for (int i = 0; i < 8; ++i) {
        miner.set_work(make_block_template(chain, test_key("miner").public_key(), {}, kTestGenesisTime + 60 * (i + 1)));
        std::optional<Block> solved;
        for (int wait = 0; wait < 5000 && !solved; ++wait) {
            solved = miner.take_solved();
            if (!solved) std::this_thread::sleep_for(1ms);
        }
        ASSERT_TRUE(solved.has_value());
        ASSERT_TRUE(chain.accept_block(*solved, kTestNow).accepted());
        std::this_thread::sleep_for(5ms);  // the other threads find out it's solved
        EXPECT_FALSE(miner.take_solved().has_value());  // only the first solution is kept
    }
    EXPECT_EQ(chain.height(), 8);
    EXPECT_GT(miner.hashes(), 0u);
    miner.set_work(std::nullopt);  // paused: nothing more turns up
    std::this_thread::sleep_for(5ms);
    EXPECT_FALSE(miner.take_solved().has_value());
}