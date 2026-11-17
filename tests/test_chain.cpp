#include "core/chain.h"
#include "core/miner.h"
#include "core/pow.h"
#include "core/validation.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

using namespace bound;

namespace {

// Milestone 4: the header rules and the block tree, through Chainstate::accept_header. Headers
// come without transactions, so the active chain stays at genesis throughout.
class BlockTreeTest : public ::testing::Test {
protected:
    // A mined header on `parent` with the bits the rules require. `salt` stands in for a merkle
    // root, so siblings differ.
    BlockHeader header_on(const BlockIndex& parent, std::uint32_t time, std::string_view salt = "") const {
        BlockHeader h;
        h.prev_hash = parent.hash;
        h.merkle_root = uint256(sha256d(bytes_of(std::string(salt) + std::to_string(parent.height + 1))));
        h.time = time;
        h.bits = next_work_required(parent, params);
        return mine(h, params).value();
    }

    BlockHeader next(std::uint32_t time) const { return header_on(*last, time); }

    // The same header with a nonce that fails proof of work.
    BlockHeader unsolved(BlockHeader h) const {
        while (check_proof_of_work(h, params)) ++h.nonce;
        return h;
    }

    // accept_header; an accepted header becomes `last`.
    AcceptResult accept(const BlockHeader& h, std::int64_t now = kTestNow) {
        const AcceptResult result = chain.accept_header(h, now);
        if (result == AcceptResult::Accepted) last = chain.find(h.hash());
        return result;
    }

    // Appends `count` headers after `last`, with times first_time, first_time + spacing, ...
    void extend(int count, std::uint32_t first_time, std::uint32_t spacing) {
        for (int i = 0; i < count; ++i) {
            const std::uint32_t time = first_time + static_cast<std::uint32_t>(i) * spacing;
            ASSERT_EQ(accept(next(time)), AcceptResult::Accepted) << "i=" << i;
        }
    }

    static constexpr std::uint32_t kT0 = kTestGenesisTime;
    const ConsensusParams params = easy_params();
    Chainstate chain{params};
    const BlockIndex* last = &chain.tip();
};

}  // namespace

TEST_F(BlockTreeTest, StartsAtGenesis) {
    const BlockIndex& genesis = chain.tip();
    EXPECT_EQ(chain.height(), 0);
    EXPECT_EQ(genesis.hash, params.genesis_hash);
    EXPECT_EQ(genesis.header, params.genesis);
    EXPECT_EQ(genesis.parent, nullptr);
    EXPECT_EQ(chain.find(params.genesis_hash), &genesis);
    EXPECT_EQ(genesis.chain_work, block_proof(params.genesis.bits));
    EXPECT_TRUE(genesis.have_data);
    EXPECT_TRUE(genesis.linked);
    EXPECT_FALSE(genesis.invalid);
    EXPECT_EQ(genesis.median_time_past(params.median_time_span), kT0);
}

TEST_F(BlockTreeTest, HeadersJoinTheTreeButNotTheActiveChain) {
    extend(5, kT0 + 60, 60);
    EXPECT_EQ(last->height, 5);
    EXPECT_EQ(last->ancestor(0), &chain.tip());
    EXPECT_EQ(last->parent->height, 4);
    EXPECT_FALSE(last->have_data);
    EXPECT_FALSE(last->linked);
    EXPECT_EQ(chain.height(), 0);  // more work, but there are no transactions to connect
}

TEST_F(BlockTreeTest, ChainWorkAddsUp) {
    extend(3, kT0 + 60, 60);
    EXPECT_EQ(last->chain_work, arith_uint256(4 * 2));  // genesis plus 3 blocks, each block_proof 2
}

TEST_F(BlockTreeTest, RejectsDuplicates) {
    const BlockHeader h = next(kT0 + 60);
    ASSERT_EQ(accept(h), AcceptResult::Accepted);
    EXPECT_EQ(accept(h), AcceptResult::Duplicate);
    EXPECT_EQ(accept(params.genesis), AcceptResult::Duplicate);
}

TEST_F(BlockTreeTest, RejectsBadProofOfWork) {
    EXPECT_EQ(accept(unsolved(next(kT0 + 60))), AcceptResult::BadProofOfWork);

    BlockHeader too_easy = next(kT0 + 60);
    too_easy.bits = 0x2100ffff;  // target above pow_limit: invalid whatever the hash
    EXPECT_EQ(accept(too_easy), AcceptResult::BadProofOfWork);
}

TEST_F(BlockTreeTest, ChecksProofOfWorkBeforeChainContext) {
    BlockHeader orphan = next(kT0 + 60);
    orphan.prev_hash = uint256(sha256d(bytes_of("unknown parent")));
    EXPECT_EQ(accept(unsolved(orphan)), AcceptResult::BadProofOfWork);
}

TEST_F(BlockTreeTest, RejectsUnknownParents) {
    BlockHeader orphan = next(kT0 + 60);
    orphan.prev_hash = uint256(sha256d(bytes_of("unknown parent")));
    orphan = mine(orphan, params).value();
    EXPECT_EQ(accept(orphan), AcceptResult::UnknownParent);
}

TEST_F(BlockTreeTest, AcceptsForks) {
    const BlockIndex& genesis = chain.tip();
    const BlockHeader a = header_on(genesis, kT0 + 60, "a");
    const BlockHeader b = header_on(genesis, kT0 + 60, "b");
    EXPECT_EQ(accept(a), AcceptResult::Accepted);
    EXPECT_EQ(accept(b), AcceptResult::Accepted);
    EXPECT_EQ(genesis.children.size(), 2u);
    EXPECT_EQ(chain.find(a.hash())->height, 1);
    EXPECT_EQ(chain.find(b.hash())->height, 1);
}

TEST_F(BlockTreeTest, RejectsWrongDifficulty) {
    BlockHeader h = next(kT0 + 60);
    h.bits = 0x207ffffe;  // valid proof of work for its own bits, but not the bits the rules give
    h = mine(h, params).value();
    EXPECT_EQ(accept(h), AcceptResult::BadDifficulty);
}

TEST_F(BlockTreeTest, MedianTimePastTakesUpperMiddle) {
    ASSERT_EQ(accept(next(kT0 + 100)), AcceptResult::Accepted);
    EXPECT_EQ(last->median_time_past(params.median_time_span), kT0 + 100);  // {T, T+100}: two values, upper one
    ASSERT_EQ(accept(next(kT0 + 101)), AcceptResult::Accepted);
    EXPECT_EQ(last->median_time_past(params.median_time_span), kT0 + 100);  // {T, T+100, T+101}
}

TEST_F(BlockTreeTest, MedianTimePastOnlySeesTheWindow) {
    extend(15, kT0 + 10, 10);  // heights 1..15 at T+10 .. T+150
    // Window = heights 5..15 = T+50 .. T+150.
    EXPECT_EQ(last->median_time_past(params.median_time_span), kT0 + 100);
}

TEST_F(BlockTreeTest, TimeMustExceedTheParentsMedianTimePast) {
    extend(10, kT0 + 1, 1);  // heights 1..10 at T+1 .. T+10
    ASSERT_EQ(last->median_time_past(params.median_time_span), kT0 + 5);
    EXPECT_EQ(accept(next(kT0 + 5)), AcceptResult::TimeTooOld);
    // Older than the tip is fine, as long as it's after the median.
    EXPECT_EQ(accept(next(kT0 + 6)), AcceptResult::Accepted);

    // On a fork, what counts is the median time past of the fork's own parent.
    const BlockIndex& two = *last->ancestor(2);  // heights 0..2 at T, T+1, T+2: median T+1
    EXPECT_EQ(accept(header_on(two, kT0 + 1, "fork")), AcceptResult::TimeTooOld);
    EXPECT_EQ(accept(header_on(two, kT0 + 2, "fork")), AcceptResult::Accepted);
}

TEST_F(BlockTreeTest, TimeMayNotRunTooFarAhead) {
    const std::int64_t now = kT0 + 1000;
    const auto limit = static_cast<std::uint32_t>(now + params.max_future_block_time);
    const BlockIndex& genesis = chain.tip();
    EXPECT_EQ(accept(header_on(genesis, limit, "at the limit"), now), AcceptResult::Accepted);
    const BlockHeader early = header_on(genesis, limit + 1, "past it");
    EXPECT_EQ(accept(early, now), AcceptResult::TimeTooNew);
    EXPECT_EQ(chain.find(early.hash()), nullptr);               // not remembered as bad...
    EXPECT_EQ(accept(early, now + 1), AcceptResult::Accepted);  // ...so it's fine a second later
}

TEST_F(BlockTreeTest, RejectionsLeaveTheTreeUnchanged) {
    extend(3, kT0 + 60, 60);
    const BlockHeader bad_pow = unsolved(next(kT0 + 1000));
    BlockHeader bad_bits = next(kT0 + 1000);
    bad_bits.bits = 0x207ffffe;
    bad_bits = mine(bad_bits, params).value();
    const BlockHeader too_old = next(kT0 + 1);
    EXPECT_NE(accept(bad_pow), AcceptResult::Accepted);
    EXPECT_NE(accept(bad_bits), AcceptResult::Accepted);
    EXPECT_NE(accept(too_old), AcceptResult::Accepted);
    EXPECT_EQ(chain.find(bad_pow.hash()), nullptr);
    EXPECT_EQ(chain.find(bad_bits.hash()), nullptr);
    EXPECT_EQ(chain.find(too_old.hash()), nullptr);
    EXPECT_EQ(last->children.size(), 0u);
}
