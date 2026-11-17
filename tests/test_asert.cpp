// Milestone 11: ASERT (aserti3-2d), checked three ways:
//   1. against Bitcoin Cash's official test vectors (tests/data/asert, 25,000 cases);
//   2. against a line-for-line copy of the specification's C++ on random inputs where it applies;
//   3. on real chains with ASERT rules: the difficulty follows the blocks, and a block with any
//      other bits is refused.
#include "core/arith_uint256.h"
#include "core/miner.h"
#include "core/params.h"
#include "core/pow.h"
#include "core/validation.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace bound;

namespace {

constexpr std::int64_t kSpacing = 600;
constexpr std::int64_t kHalfLife = 172'800;  // 2 days
constexpr std::uint32_t kSpecPowLimitBits = 0x1d00ffff;  // Bitcoin Cash's, under 2^224

arith_uint256 target_of(std::uint32_t bits) { return arith_uint256(target_from_compact(bits).value()); }
std::uint32_t bits_of(const arith_uint256& target) { return compact_from_target(target.to_uint256()); }

// The specification's CalculateASERT (Bitcoin Cash Node, src/pow/aserti32d.cpp), line for line.
// Its 256-bit product refTarget * factor is formed with mul_div(a, factor, 1), which is exact; the
// specification's preconditions (refTarget <= powLimit < 2^224) keep that product below 2^241.
arith_uint256 spec_asert(const arith_uint256& refTarget, std::int64_t nPowTargetSpacing, std::int64_t nTimeDiff,
                         std::int64_t nHeightDiff, const arith_uint256& powLimit, std::int64_t nHalfLife) {
    const std::int64_t exponent = ((nTimeDiff - nPowTargetSpacing * (nHeightDiff + 1)) * 65536) / nHalfLife;
    std::int64_t shifts = exponent >> 16;
    const auto frac = std::uint16_t(exponent);
    const std::uint32_t factor =
        65536 + std::uint32_t((+195766423245049ull * frac + 971821376ull * frac * frac + 5127ull * frac * frac * frac + (1ull << 47)) >> 48);
    arith_uint256 nextTarget = mul_div(refTarget, factor, 1).value();
    shifts -= 16;
    if (shifts <= 0) {
        nextTarget >>= static_cast<unsigned>(-shifts);
    } else {
        const auto nextTargetShifted = nextTarget << static_cast<unsigned>(shifts);
        if ((nextTargetShifted >> static_cast<unsigned>(shifts)) != nextTarget) {
            nextTarget = powLimit;
        } else {
            nextTarget = nextTargetShifted;
        }
    }
    if (nextTarget == 0) {
        nextTarget = arith_uint256(1);
    } else if (nextTarget > powLimit) {
        nextTarget = powLimit;
    }
    return nextTarget;
}

struct VectorFile {
    std::string name;
    std::uint64_t anchor_height = 0;
    std::int64_t anchor_parent_time = 0;
    std::uint32_t anchor_bits = 0;
    struct Line {
        std::uint64_t height;
        std::int64_t time;
        std::uint32_t bits;
    };
    std::vector<Line> lines;
};

std::string header_value(const std::string& line, const std::string& key) {
    const auto at = line.find(key);
    return at == std::string::npos ? std::string() : line.substr(at + key.size());
}

VectorFile read_vectors(const std::filesystem::path& file) {
    VectorFile v;
    v.name = file.filename().string();
    std::ifstream in(file);
    if (!in) throw std::runtime_error("can't read " + file.string());
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("##")) {
            if (const std::string s = header_value(line, "anchor height:"); !s.empty()) v.anchor_height = std::stoull(s);
            if (const std::string s = header_value(line, "anchor parent time:"); !s.empty()) v.anchor_parent_time = std::stoll(s);
            if (const std::string s = header_value(line, "anchor nBits:"); !s.empty()) v.anchor_bits = static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
            continue;
        }
        if (line.empty() || line.starts_with("#")) continue;
        std::istringstream fields(line);
        std::string iteration, height, time, bits;
        fields >> iteration >> height >> time >> bits;
        v.lines.push_back({std::stoull(height), std::stoll(time), static_cast<std::uint32_t>(std::stoul(bits, nullptr, 16))});
    }
    return v;
}

}  // namespace

// 1. Every line of every official vector file. Heights in run10 pass 2^63, so they're read
// unsigned, and only their difference from the anchor's is used, as in the specification.
TEST(Asert, MatchesBitcoinCashsTestVectors) {
    const std::filesystem::path dir = std::filesystem::path(BOUND_TEST_DATA_DIR) / "asert";
    const arith_uint256 pow_limit = target_of(kSpecPowLimitBits);
    std::size_t files = 0;
    std::size_t checked = 0;
    for (int run = 1; run <= 12; ++run) {
        const VectorFile v = read_vectors(dir / std::format("run{:02}", run));
        ++files;
        ASSERT_FALSE(v.lines.empty()) << v.name;
        const arith_uint256 anchor = target_of(v.anchor_bits);
        for (const VectorFile::Line& l : v.lines) {
            const auto height_diff = static_cast<std::int64_t>(l.height - v.anchor_height);
            const arith_uint256 got = asert_target(anchor, l.time - v.anchor_parent_time, height_diff, kSpacing, kHalfLife, pow_limit);
            ASSERT_EQ(bits_of(got), l.bits) << v.name << " height " << l.height << " time " << l.time;
            ++checked;
        }
    }
    EXPECT_EQ(files, 12u);
    EXPECT_GT(checked, 13'000u);
}

// 2. Where the specification applies, the same result as its own code, for random anchors,
// heights and times (including far off schedule, where the clamps take over).
TEST(Asert, MatchesTheSpecificationsCode) {
    const arith_uint256 pow_limit = target_of(kSpecPowLimitBits);
    std::mt19937_64 rng(20201115);
    std::uniform_int_distribution<std::uint32_t> exponent_byte(0x04, 0x1d);
    std::uniform_int_distribution<std::uint32_t> mantissa(0x008000, 0x7fffff);
    std::uniform_int_distribution<std::int64_t> height(0, 10'000'000);
    std::uniform_int_distribution<std::int64_t> lag(-(std::int64_t{1} << 40), std::int64_t{1} << 40);
    std::uniform_int_distribution<std::int64_t> small_lag(-20 * kHalfLife, 20 * kHalfLife);
    for (int i = 0; i < 200'000; ++i) {
        arith_uint256 anchor = target_of((exponent_byte(rng) << 24) | mantissa(rng));
        if (anchor > pow_limit) anchor = pow_limit;
        if (anchor == 0) anchor = 1;
        const std::int64_t h = height(rng);
        const std::int64_t t = h * kSpacing + (i % 2 == 0 ? small_lag(rng) : lag(rng));
        ASSERT_EQ(asert_target(anchor, t, h, kSpacing, kHalfLife, pow_limit), spec_asert(anchor, kSpacing, t, h, pow_limit, kHalfLife))
            << "anchor " << anchor.to_uint256().to_hex() << " time " << t << " height " << h;
    }
}

// Beyond the specification (an easy pow_limit, as the tests' is, and mainnet's: 0x1e03ffff since
// milestone 16, about 2^234, above the spec's 2^224), the arithmetic stays exact: one half-life
// behind schedule doubles the target, one ahead halves it, and the clamps hold.
TEST(Asert, BehavesWithAnEasyPowLimit) {
    const arith_uint256 easy = target_of(0x207fffff);  // ~2^255, where the spec's 256 bits would overflow
    const arith_uint256 anchor = target_of(0x1f00ffff);
    const auto at = [&](std::int64_t lag) { return asert_target(anchor, lag + kSpacing, 0, kSpacing, kHalfLife, easy); };
    EXPECT_EQ(at(0), anchor);  // on schedule (counting the anchor's own block): the anchor's target
    EXPECT_EQ(at(kHalfLife), anchor << 1);
    EXPECT_EQ(at(-kHalfLife), anchor >> 1);
    EXPECT_EQ(at(15 * kHalfLife), anchor << 15);
    EXPECT_EQ(at(100 * kHalfLife), easy);  // clamped at the limit
    EXPECT_EQ(at(-300 * kHalfLife), arith_uint256(1));  // and at 1
    // Later never makes it harder, earlier never easier.
    arith_uint256 previous = 0;
    for (std::int64_t lag = -3 * kHalfLife; lag <= 3 * kHalfLife; lag += 977) {
        const arith_uint256 now = at(lag);
        EXPECT_GE(now, previous) << lag;
        previous = now;
    }
    // With the anchor at the limit itself, as a provisional genesis has it.
    EXPECT_EQ(asert_target(easy, 50 * kHalfLife, 0, kSpacing, kHalfLife, easy), easy);

    EXPECT_THROW(asert_target(0, 0, 0, kSpacing, kHalfLife, easy), std::invalid_argument);
    EXPECT_THROW(asert_target(easy + 1, 0, 0, kSpacing, kHalfLife, easy), std::invalid_argument);
    EXPECT_THROW(asert_target(anchor, 0, -1, kSpacing, kHalfLife, easy), std::invalid_argument);
    EXPECT_THROW(asert_target(anchor, std::int64_t{1} << 48, 0, kSpacing, kHalfLife, easy), std::invalid_argument);
}

namespace {

// The unit-test chain with ASERT rules, and a short half-life so a few blocks show its effect.
ConsensusParams asert_params() {
    ConsensusParams p = easy_params();
    p.asert_half_life = 3600;
    return p;
}

// A block on the tip at `time`, with whatever bits the rules ask for (or `bits` if given).
Block block_at(const Chainstate& chain, std::uint32_t time, std::optional<std::uint32_t> bits = std::nullopt) {
    Block b = make_block_template(chain, test_key("miner").public_key(), {}, time).value();
    if (bits) b.header.bits = *bits;
    return mine_block(std::move(b), chain.params());
}

}  // namespace

// 3. On a chain: blocks faster than the schedule make the next ones harder, slower ones easier,
// every block's bits are exactly asert_target's, and other bits are refused.
TEST(Asert, SetsEveryBlocksDifficulty) {
    const ConsensusParams p = asert_params();
    Chainstate chain(p);
    const arith_uint256 anchor = target_of(p.genesis.bits);
    std::uint32_t time = p.genesis.time;

    // On schedule: the same target block after block.
    for (int i = 0; i < 5; ++i) {
        time += static_cast<std::uint32_t>(p.target_spacing);
        ASSERT_TRUE(chain.accept_block(block_at(chain, time), kTestNow).accepted());
    }
    const std::uint32_t steady = next_work_required(chain.tip(), p);
    time += static_cast<std::uint32_t>(p.target_spacing);
    ASSERT_TRUE(chain.accept_block(block_at(chain, time), kTestNow).accepted());
    EXPECT_EQ(next_work_required(chain.tip(), p), steady);

    // A burst of blocks a second apart: harder each time.
    std::uint32_t previous = steady;
    for (int i = 0; i < 20; ++i) {
        ++time;
        ASSERT_TRUE(chain.accept_block(block_at(chain, time), kTestNow).accepted());
        const std::uint32_t bits = next_work_required(chain.tip(), p);
        EXPECT_LT(target_of(bits), target_of(previous));
        previous = bits;
    }
    // Then a long gap: easier, all the way back to the limit.
    time += 10 * static_cast<std::uint32_t>(p.asert_half_life);
    ASSERT_TRUE(chain.accept_block(block_at(chain, time), kTestNow).accepted());
    EXPECT_EQ(next_work_required(chain.tip(), p), p.pow_limit_bits);

    // Every block's bits are what asert_target says for its parent.
    for (int h = 1; h <= chain.height(); ++h) {
        const BlockIndex& parent = chain.at(h - 1);
        const arith_uint256 want = asert_target(anchor, std::int64_t{parent.header.time} - p.genesis.time, parent.height,
                                                p.target_spacing, p.asert_half_life, arith_uint256(p.pow_limit));
        EXPECT_EQ(chain.at(h).header.bits, bits_of(want)) << "height " << h;
    }
    // The plain-values form (for headers presync) agrees.
    const BlockIndex& tip = chain.tip();
    EXPECT_EQ(next_work_required(tip.height, tip.header.bits, tip.header.time, [](int) -> std::int64_t { throw std::logic_error("unused"); }, p),
              next_work_required(tip, p));
}

TEST(Asert, RefusesOtherBits) {
    const ConsensusParams p = asert_params();
    Chainstate chain(p);
    std::uint32_t time = p.genesis.time;
    for (int i = 0; i < 10; ++i) {
        ++time;  // fast blocks, so the required bits move away from the limit
        ASSERT_TRUE(chain.accept_block(block_at(chain, time), kTestNow).accepted());
    }
    const std::uint32_t required = next_work_required(chain.tip(), p);
    ASSERT_NE(required, p.pow_limit_bits);
    // The parent's bits, or the limit's: proof of work that meets them, but the wrong difficulty.
    EXPECT_FALSE(chain.accept_block(block_at(chain, time + 1, chain.tip().header.bits), kTestNow).accepted());
    EXPECT_FALSE(chain.accept_block(block_at(chain, time + 1, p.pow_limit_bits), kTestNow).accepted());
    EXPECT_TRUE(chain.accept_block(block_at(chain, time + 1), kTestNow).accepted());
}

// Mainnet's rules, as the project owner chose them.
TEST(Asert, IsMainnetsRule) {
    const ConsensusParams& main = mainnet_params();
    EXPECT_EQ(main.asert_half_life, 2 * 24 * 60 * 60);
    EXPECT_EQ(testnet_params().asert_half_life, 0);  // testnet keeps its daily retarget
    EXPECT_EQ(regtest_params().asert_half_life, 0);
    // The first block after the genesis, a block's time later: the genesis's own difficulty but for
    // the specification's one-block offset for a genesis anchor (2^(-600/172800), about 0.24%).
    Chainstate chain(main);
    const std::uint32_t first = next_work_required(chain.tip(), main);
    EXPECT_EQ(first, bits_of(asert_target(target_of(main.genesis.bits), 0, 0, main.target_spacing, main.asert_half_life,
                                          arith_uint256(main.pow_limit))));
    EXPECT_LT(target_of(first), target_of(main.genesis.bits));
    EXPECT_GT(target_of(first), target_of(main.genesis.bits) - (target_of(main.genesis.bits) >> 8));
}
