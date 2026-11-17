#include "core/block.h"
#include "core/merkle.h"
#include "test_util.h"
#include "util/hex.h"
#include "util/serialize.h"

#include <gtest/gtest.h>

#include <vector>

using namespace bound;

namespace {

// A coinbase at height 1 plus Alice's signed payment. The nonce is arbitrary: these tests
// don't need valid proof of work.
Block sample_block() {
    Transaction coinbase;
    coinbase.outputs = {{5 * kCoin, test_key("alice").public_key()}};
    coinbase.lock_time = 1;

    Transaction payment = rent_payment();
    sign_all_inputs(payment, test_key("alice"));

    Block b;
    b.header.prev_hash = test_hash("parent");
    b.header.time = 1'800'000'000;
    b.header.bits = 0x207fffff;
    b.header.nonce = 42;
    b.txs = {coinbase, payment};
    b.header.merkle_root = b.compute_merkle_root();
    return b;
}

}  // namespace

TEST(Block, MerkleRootCommitsToWtxids) {
    const Block b = sample_block();
    EXPECT_EQ(b.compute_merkle_root().to_hex(), "648aafc29932e1910c0b2eb3ed07518153fa061288b892034e38755f0eecb261");
    const std::vector<uint256> wtxids = {b.txs[0].wtxid(), b.txs[1].wtxid()};
    EXPECT_EQ(b.compute_merkle_root(), merkle_root(wtxids));
}

TEST(Block, SingleTransactionRootIsItsWtxid) {
    Block b = sample_block();
    b.txs.resize(1);
    EXPECT_EQ(b.compute_merkle_root(), b.txs[0].wtxid());
}

TEST(Block, Serialization) {
    const Block b = sample_block();
    const Bytes bytes = b.serialize();
    EXPECT_EQ(bytes.size(), 427u);
    EXPECT_EQ(to_hex(sha256(bytes)), "a2122c3d1e4eb7953d66edd55179dbf0fe5e5225a1ea35f3c416be0665604c4d");

    ByteWriter layout;  // header, transaction count, transactions
    layout.bytes(b.header.serialize());
    layout.compact_size(2);
    b.txs[0].write(layout);
    b.txs[1].write(layout);
    EXPECT_EQ(bytes, layout.data());
}

TEST(Block, RoundTrips) {
    const Block b = sample_block();
    EXPECT_EQ(Block::deserialize(b.serialize()), b);
}

TEST(Block, DeserializeRejectsMalformedInput) {
    const Bytes bytes = sample_block().serialize();
    EXPECT_THROW(Block::deserialize(ByteSpan(bytes).first(bytes.size() - 1)), DeserializeError);

    Bytes trailing = bytes;
    trailing.push_back(0);
    EXPECT_THROW(Block::deserialize(trailing), DeserializeError);

    ByteWriter huge;  // a header, then a claim of 4,294,967,295 transactions
    huge.bytes(sample_block().header.serialize());
    huge.compact_size(0xffffffff);
    EXPECT_THROW(Block::deserialize(huge.data()), DeserializeError);
}

TEST(Block, HashIsTheHeaderHash) {
    const Block b = sample_block();
    EXPECT_EQ(b.hash(), b.header.hash());
}
