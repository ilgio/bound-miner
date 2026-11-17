#include "core/chain.h"
#include "core/validation.h"
#include "net/headerssync.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace bound;

namespace {

std::vector<BlockHeader> headers_of(const Chainstate& chain, int from, int to) {
    std::vector<BlockHeader> headers;
    for (int h = from; h <= to; ++h) headers.push_back(chain.at(h).header);
    return headers;
}

// Three chains from the same genesis: ours (30 blocks), theirs and another (40 each).
class HeadersSyncTest : public ::testing::Test {
protected:
    HeadersSyncTest() {
        mine_blocks(ours, test_key("ours").public_key(), 30);
        mine_blocks(theirs, test_key("theirs").public_key(), 40);
        mine_blocks(another, test_key("another").public_key(), 40);
    }
    // Their chain needs as much work as ours before any of it is stored: it gets there at #30.
    HeadersSync make(int period = 2, std::size_t buffer = 4) const {
        return HeadersSync(ours.at(0), ours.tip().chain_work, params, {period, buffer}, 0x0123456789abcdefULL, kTestNow);
    }

    const ConsensusParams params = easy_params();
    Chainstate ours{params};
    Chainstate theirs{params};
    Chainstate another{params};
};

}  // namespace

TEST_F(HeadersSyncTest, AChainWithTooLittleWorkIsDropped) {
    HeadersSync sync = make();
    const HeadersSync::Result first = sync.process(headers_of(theirs, 1, 10), kTestNow);
    EXPECT_TRUE(first.to_store.empty());
    EXPECT_FALSE(first.invalid);
    ASSERT_FALSE(first.locator.empty());
    EXPECT_EQ(first.locator[0], theirs.at(10).hash);  // "carry on after #10"
    EXPECT_EQ(sync.phase(), HeadersSync::Phase::Presync);
    EXPECT_TRUE(sync.process(headers_of(theirs, 11, 20), kTestNow).to_store.empty());
    EXPECT_EQ(sync.presync_height(), 20);

    const HeadersSync::Result end = sync.process({}, kTestNow);  // the peer has no more
    EXPECT_EQ(sync.phase(), HeadersSync::Phase::Failed);
    EXPECT_TRUE(end.to_store.empty());
    EXPECT_FALSE(end.invalid);
    EXPECT_TRUE(end.locator.empty());
}

TEST_F(HeadersSyncTest, EnoughWorkIsDownloadedAgainThenStored) {
    HeadersSync sync = make(2, 4);
    sync.process(headers_of(theirs, 1, 10), kTestNow);
    sync.process(headers_of(theirs, 11, 20), kTestNow);
    const HeadersSync::Result switched = sync.process(headers_of(theirs, 21, 35), kTestNow);
    EXPECT_EQ(sync.phase(), HeadersSync::Phase::Redownload);
    EXPECT_EQ(sync.presync_height(), 30);  // enough at #30; the rest of that batch isn't needed
    EXPECT_TRUE(switched.to_store.empty());
    EXPECT_EQ(switched.locator, block_locator(ours.at(0)));  // from the fork again

    HeadersSync::Result r = sync.process(headers_of(theirs, 1, 10), kTestNow);
    EXPECT_EQ(r.to_store, headers_of(theirs, 1, 6));  // the last 4 held back
    r = sync.process(headers_of(theirs, 11, 20), kTestNow);
    EXPECT_EQ(r.to_store, headers_of(theirs, 7, 16));
    r = sync.process(headers_of(theirs, 21, 32), kTestNow);  // the work again at #30: all of it goes
    EXPECT_EQ(r.to_store, headers_of(theirs, 17, 32));
    EXPECT_EQ(sync.phase(), HeadersSync::Phase::Done);
}

// A peer that shows a chain with the work, then sends a different one to be stored.
TEST_F(HeadersSyncTest, AChainSwappedForTheSecondDownloadIsCaught) {
    HeadersSync sync = make(1, 100);  // every header committed, nothing released before the end
    sync.process(headers_of(theirs, 1, 30), kTestNow);
    ASSERT_EQ(sync.phase(), HeadersSync::Phase::Redownload);
    const HeadersSync::Result r = sync.process(headers_of(another, 1, 30), kTestNow);
    EXPECT_EQ(sync.phase(), HeadersSync::Phase::Failed);
    EXPECT_EQ(r.note, "the headers changed between the two downloads");
    EXPECT_FALSE(r.invalid);  // an honest peer may have reorganised in between
    EXPECT_TRUE(r.to_store.empty());
}

TEST_F(HeadersSyncTest, BrokenRulesAreReported) {
    const auto check = [&](std::vector<BlockHeader> headers, const char* expected) {
        HeadersSync sync = make();
        const HeadersSync::Result r = sync.process(headers, kTestNow);
        EXPECT_TRUE(r.invalid) << expected;
        EXPECT_EQ(r.note, expected);
        EXPECT_EQ(sync.phase(), HeadersSync::Phase::Failed);
    };
    std::vector<BlockHeader> wrong_bits = headers_of(theirs, 1, 3);
    wrong_bits[2].bits = 0x207ffffe;
    wrong_bits[2] = mine(wrong_bits[2], params).value();
    check(wrong_bits, "wrong difficulty");

    BlockHeader old = theirs.at(1).header;
    old.time = kTestGenesisTime;  // not after genesis's
    check({mine(old, params).value()}, "time too old");

    check({theirs.at(1).header, theirs.at(3).header}, "headers that don't follow on from each other");

    BlockHeader no_work = theirs.at(1).header;
    while (check_proof_of_work(no_work, params)) ++no_work.nonce;
    check({no_work}, "bad proof of work");
}

TEST_F(HeadersSyncTest, NotThePeersFault) {
    // A header too far ahead of our clock fails the sync without blaming anyone.
    HeadersSync early = make();
    const HeadersSync::Result r = early.process(headers_of(theirs, 1, 5), kTestGenesisTime + 60 * 3 - 2 * 60 * 60);
    EXPECT_FALSE(r.invalid);
    EXPECT_EQ(early.phase(), HeadersSync::Phase::Failed);

    // A batch that doesn't carry on from the last one (a new tip announced meanwhile) is ignored.
    HeadersSync sync = make();
    sync.process(headers_of(theirs, 1, 5), kTestNow);
    const HeadersSync::Result skipped = sync.process(headers_of(theirs, 8, 9), kTestNow);
    EXPECT_TRUE(skipped.locator.empty());
    EXPECT_EQ(sync.phase(), HeadersSync::Phase::Presync);
    EXPECT_EQ(sync.presync_height(), 5);
}

// Presync has no block tree for the chain it checks, so it works out each retarget from the times
// it remembers. With a 20-block period and blocks twice as fast as the target, the difficulty
// changes at #20, #40, #60 and #80; from #40 on, the window's first block is one presync saw.
TEST(HeadersSyncRetarget, ChecksDifficultyLikeTheTree) {
    ConsensusParams params = easy_params();
    params.retarget_interval = 20;
    Chainstate fast(params);
    const PublicKey miner = test_key("fast").public_key();
    for (int h = 1; h <= 100; ++h) {
        const Block block = block_on(fast.tip(), params, miner, kTestGenesisTime + 30 * static_cast<std::uint32_t>(h));
        ASSERT_TRUE(fast.accept_block(block, kTestNow).accepted()) << h;
    }
    ASSERT_NE(fast.at(80).header.bits, fast.at(79).header.bits);
    const arith_uint256 never = fast.tip().chain_work + fast.tip().chain_work;

    for (const int start : {0, 30}) {
        HeadersSync sync(fast.at(start), never, params, {}, 7, kTestNow);
        for (int from = start + 1; from <= 100; from += 13) {
            const HeadersSync::Result r = sync.process(headers_of(fast, from, std::min(100, from + 12)), kTestNow);
            ASSERT_FALSE(r.invalid) << "from " << start << ": " << r.note;
        }
        EXPECT_EQ(sync.presync_height(), 100);
        EXPECT_EQ(sync.phase(), HeadersSync::Phase::Presync);
    }

    std::vector<BlockHeader> stale = headers_of(fast, 1, 60);
    stale[59].bits = stale[58].bits;  // #60 keeping the old difficulty
    stale[59] = mine(stale[59], params).value();
    HeadersSync sync(fast.at(0), never, params, {}, 7, kTestNow);
    const HeadersSync::Result r = sync.process(stale, kTestNow);
    EXPECT_TRUE(r.invalid);
    EXPECT_EQ(r.note, "wrong difficulty");
}
