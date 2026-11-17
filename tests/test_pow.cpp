#include "core/chain.h"
#include "core/params.h"
#include "core/pow.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>

using namespace bound;

namespace {

// Bitcoin mainnet's pow limit. A function, not a global: test setup must not call your code
// before main(), or one bug would take down the whole test binary.
uint256 bitcoin_pow_limit() { return U("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); }

std::string decode(std::uint32_t bits) {
    const std::optional<uint256> t = target_from_compact(bits);
    return t ? t->to_hex() : "nullopt";
}

}  // namespace

// Cases from Bitcoin Core's arith_uint256_tests (bignum_SetCompact), plus overflow edges.
TEST(Compact, Decode) {
    const std::string zero(64, '0');
    for (std::uint32_t bits : {0x00000000u, 0x00123456u, 0x01003456u, 0x02000056u, 0x03000000u, 0x04000000u,
                               0x00923456u, 0x01803456u, 0x02800056u, 0x03800000u, 0x04800000u}) {
        EXPECT_EQ(decode(bits), zero) << std::hex << "bits=0x" << bits;
    }
    EXPECT_EQ(decode(0x01123456), "0000000000000000000000000000000000000000000000000000000000000012");
    EXPECT_EQ(decode(0x02123456), "0000000000000000000000000000000000000000000000000000000000001234");
    EXPECT_EQ(decode(0x03123456), "0000000000000000000000000000000000000000000000000000000000123456");
    EXPECT_EQ(decode(0x04123456), "0000000000000000000000000000000000000000000000000000000012345600");
    EXPECT_EQ(decode(0x05009234), "0000000000000000000000000000000000000000000000000000000092340000");
    EXPECT_EQ(decode(0x20123456), "1234560000000000000000000000000000000000000000000000000000000000");
    EXPECT_EQ(decode(0x1d00ffff), "00000000ffff0000000000000000000000000000000000000000000000000000");
    EXPECT_EQ(decode(0x22000001), "0100000000000000000000000000000000000000000000000000000000000000");
    EXPECT_EQ(decode(0x2100ffff), "ffff000000000000000000000000000000000000000000000000000000000000");
}

TEST(Compact, DecodeRejectsNegative) {
    EXPECT_EQ(decode(0x01fedcba), "nullopt");
    EXPECT_EQ(decode(0x04923456), "nullopt");
}

TEST(Compact, DecodeRejectsOverflow) {
    for (std::uint32_t bits : {0xff123456u, 0x23000001u, 0x22000100u, 0x21010000u}) {
        EXPECT_EQ(decode(bits), "nullopt") << std::hex << "bits=0x" << bits;
    }
}

TEST(Compact, Encode) {
    struct Case {
        const char* target;
        std::uint32_t bits;
    };
    const Case cases[] = {
        {"0000000000000000000000000000000000000000000000000000000000000000", 0x00000000},
        {"0000000000000000000000000000000000000000000000000000000000000012", 0x01120000},
        {"0000000000000000000000000000000000000000000000000000000000000080", 0x02008000},  // sign bit avoided
        {"0000000000000000000000000000000000000000000000000000000000001234", 0x02123400},
        {"0000000000000000000000000000000000000000000000000000000000123456", 0x03123456},
        {"0000000000000000000000000000000000000000000000000000000012345600", 0x04123456},
        {"0000000000000000000000000000000000000000000000000000000092340000", 0x05009234},
        {"1234560000000000000000000000000000000000000000000000000000000000", 0x20123456},
        {"00000000ffff0000000000000000000000000000000000000000000000000000", 0x1d00ffff},
        {"0000000000000000000000000000000000000000000000000000000123456789", 0x05012345},  // lossy
        {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 0x2100ffff},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(compact_from_target(U(c.target)), c.bits) << "target=" << c.target;
    }
}

TEST(Compact, CanonicalEncodingsRoundTrip) {
    for (std::uint32_t bits : {0x1d00ffffu, 0x1f00ffffu, 0x2000ffffu, 0x207fffffu, 0x03123456u}) {
        EXPECT_EQ(compact_from_target(target_from_compact(bits).value()), bits) << std::hex << "bits=0x" << bits;
    }
}

TEST(Compact, EncodingIsLossy) {
    const uint256 precise = U("0000000000000000000000000000000000000000000000000000000123456789");
    EXPECT_EQ(decode(compact_from_target(precise)),
              "0000000000000000000000000000000000000000000000000000000123450000");
}

TEST(Pow, BitcoinGenesisMeetsItsTarget) {
    EXPECT_TRUE(check_proof_of_work(U("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f"),
                                    0x1d00ffff, bitcoin_pow_limit()));
}

TEST(Pow, TargetIsInclusive) {
    const uint256 target = target_from_compact(0x1d00ffff).value();
    EXPECT_TRUE(check_proof_of_work(target, 0x1d00ffff, bitcoin_pow_limit()));
    EXPECT_FALSE(check_proof_of_work(U("00000000ffff0000000000000000000000000000000000000000000000000001"),
                                     0x1d00ffff, bitcoin_pow_limit()));  // target + 1
    EXPECT_TRUE(check_proof_of_work(uint256{}, 0x1d00ffff, bitcoin_pow_limit()));
}

TEST(Pow, RejectsInvalidTargets) {
    const uint256 zero_hash;
    EXPECT_FALSE(check_proof_of_work(zero_hash, 0x00000000, bitcoin_pow_limit()));  // target 0
    EXPECT_FALSE(check_proof_of_work(zero_hash, 0x04923456, bitcoin_pow_limit()));  // negative
    EXPECT_FALSE(check_proof_of_work(zero_hash, 0xff123456, bitcoin_pow_limit()));  // overflow
    EXPECT_FALSE(check_proof_of_work(zero_hash, 0x1d01ffff, bitcoin_pow_limit()));  // easier than the limit
}

// Milestone 4. Expected values computed independently (Python big integers).

TEST(BlockProof, MatchesBitcoinsGenesisWork) {
    EXPECT_EQ(block_proof(0x1d00ffff), arith_uint256(0x100010001ULL));  // Bitcoin's chainwork at height 0
}

TEST(BlockProof, Values) {
    EXPECT_EQ(block_proof(0x1f00ffff), arith_uint256(65537));  // the testnet minimum: ~65k hashes
    EXPECT_EQ(block_proof(0x1e3fffc0), arith_uint256(262148));  // a quarter of that target: 4x the work
    EXPECT_EQ(block_proof(0x207fffff), arith_uint256(2));
    EXPECT_EQ(block_proof(0x00000000), arith_uint256(0));  // target 0
    EXPECT_EQ(block_proof(0x04923456), arith_uint256(0));  // negative
}

// Bitcoin Core's pow_tests (get_next_work, _pow_limit, _lower_limit_actual,
// _upper_limit_actual): the same formula, with Bitcoin's two-week timespan and limit.
TEST(Retarget, MatchesBitcoinCore) {
    constexpr std::int64_t kTwoWeeks = 14 * 24 * 60 * 60;
    EXPECT_EQ(retarget(0x1d00ffff, 1262152739 - 1261130161, kTwoWeeks, bitcoin_pow_limit()), 0x1d00d86au);
    EXPECT_EQ(retarget(0x1d00ffff, 1233061996 - 1231006505, kTwoWeeks, bitcoin_pow_limit()), 0x1d00ffffu);  // capped
    EXPECT_EQ(retarget(0x1c05a3f4, 1279297671 - 1279008237, kTwoWeeks, bitcoin_pow_limit()), 0x1c0168fdu);  // 4x harder at most
    EXPECT_EQ(retarget(0x1c387f6f, 1269211443 - 1263163443, kTwoWeeks, bitcoin_pow_limit()), 0x1d00e1fdu);  // 4x easier at most
}

TEST(Retarget, ClampsToFourTimesEitherWay) {
    const uint256 limit = testnet_params().pow_limit;
    EXPECT_EQ(retarget(0x1f00ffff, 86400 / 4, 86400, limit), 0x1e3fffc0u);  // 4x too fast
    EXPECT_EQ(retarget(0x1f00ffff, 0, 86400, limit), 0x1e3fffc0u);          // instant: still only 4x
    EXPECT_EQ(retarget(0x1f00ffff, -1000, 86400, limit), 0x1e3fffc0u);      // negative: the same
    EXPECT_EQ(retarget(0x1e0ffff0, 864000, 86400, limit), 0x1e3fffc0u);     // 10x too slow: only 4x easier
    EXPECT_EQ(retarget(0x1e0ffff0, 86400, 86400, limit), 0x1e0ffff0u);      // on schedule: unchanged
}

TEST(Retarget, NeverEasierThanThePowLimit) {
    EXPECT_EQ(retarget(0x1e3fffc0, 864000, 86400, testnet_params().pow_limit), 0x1f00ffffu);
}

TEST(Retarget, ExactWhenTheProductNeedsMoreThan256Bits) {
    // The target is about 2^248; times 4 * 86400 that's a 267-bit intermediate.
    EXPECT_EQ(retarget(0x2000ffff, 4 * 86400, 86400, target_from_compact(0x207fffff).value()), 0x2003fffcu);
}

TEST(Retarget, RejectsAnUnusableExpectedTimespan) {
    EXPECT_THROW(retarget(0x1f00ffff, 60, 0, testnet_params().pow_limit), std::invalid_argument);
}

TEST(Difficulty, IsRelativeToThePowLimit) {
    EXPECT_DOUBLE_EQ(difficulty(0x1f00ffff, testnet_params()), 1.0);
    EXPECT_DOUBLE_EQ(difficulty(0x1e3fffc0, testnet_params()), 4.0);
}

namespace {

// next_work_required over hand-built block trees: chosen timestamps, no proof of work. Testnet
// rules with a 10-block period, so the periods are short enough to write out.
class NextWorkTest : public ::testing::Test {
protected:
    NextWorkTest() {
        params.retarget_interval = 10;
        BlockIndex& genesis = nodes.emplace_back();
        genesis.header.time = kT0;
        genesis.header.bits = params.pow_limit_bits;
    }

    // Appends a block at `time` with the bits the rules give it. A deque never moves its
    // elements, so parent pointers stay valid.
    const BlockIndex& add(std::uint32_t time) {
        BlockIndex& parent = nodes.back();
        BlockIndex& node = nodes.emplace_back();
        node.parent = &parent;
        node.height = parent.height + 1;
        node.header.time = time;
        node.header.bits = next_work_required(parent, params);
        return node;
    }

    static constexpr std::uint32_t kT0 = 1'800'000'000;
    ConsensusParams params = testnet_params();
    std::deque<BlockIndex> nodes;
};

}  // namespace

TEST_F(NextWorkTest, ChangesOnlyAtPeriodStarts) {
    for (std::uint32_t i = 1; i <= 9; ++i) EXPECT_EQ(add(kT0 + 30 * i).header.bits, 0x1f00ffffu) << "height " << i;
    EXPECT_EQ(add(kT0 + 300).header.bits, 0x1e7fff80u);  // twice as fast as planned: twice as hard
    EXPECT_EQ(add(kT0 + 330).header.bits, 0x1e7fff80u);  // and that sticks for the whole period
}

TEST_F(NextWorkTest, FirstRetargetMeasuresFromGenesis) {
    for (std::uint32_t i = 1; i <= 9; ++i) add(kT0 + i);  // one second apart: clamped to 4x
    EXPECT_EQ(add(kT0 + 10).header.bits, 0x1e3fffc0u);
}

TEST_F(NextWorkTest, KeepsAdjustingFromTheCurrentDifficulty) {
    std::uint32_t t = kT0;
    for (int i = 1; i <= 9; ++i) add(t += 30);                       // too fast
    for (int i = 10; i <= 19; ++i) add(t += 60);                     // on schedule
    for (int i = 20; i <= 29; ++i) add(t += 240);                    // four times too slow
    EXPECT_EQ(nodes[10].header.bits, 0x1e7fff80u);                    // twice as hard
    EXPECT_EQ(nodes[20].header.bits, 0x1e7fff80u);                    // kept, not reset
    EXPECT_EQ(add(t += 60).header.bits, 0x1f00ffffu);                // 4x easier, capped at the limit
}

// The timewarp trick: stamp a period's first block as early as median time past allows. With
// Bitcoin's window, which starts at that block, the next period would look longer than it was
// and get easier. BOUND's window starts at the block before, so the trick changes nothing.
TEST_F(NextWorkTest, WindowsShareEndpointsSoTimewarpGainsNothing) {
    nodes.front().header.bits = 0x1e0ffff0;  // start above the minimum, so easier would show
    for (std::uint32_t i = 1; i <= 9; ++i) add(kT0 + 60 * i);
    EXPECT_EQ(add(kT0 + 301).header.bits, 0x1e0ffff0u);  // median time past is kT0 + 300
    for (std::uint32_t i = 11; i <= 19; ++i) add(kT0 + 60 * i);
    EXPECT_EQ(add(kT0 + 1200).header.bits, 0x1e0ffff0u);
    EXPECT_EQ(retarget(0x1e0ffff0, nodes[19].header.time - nodes[10].header.time, 600, params.pow_limit),
              0x1e165f7cu);  // what Bitcoin's window would have allowed
}
