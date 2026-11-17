#include "core/block.h"
#include "core/pow.h"
#include "test_util.h"
#include "util/hex.h"
#include "util/serialize.h"

#include <gtest/gtest.h>

using namespace bound;

namespace {

BlockHeader bitcoin_genesis() {
    BlockHeader h;
    h.version = 1;
    h.merkle_root = U("4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
    h.time = 1231006505;
    h.bits = 0x1d00ffff;
    h.nonce = 2083236893;
    return h;
}

BlockHeader bitcoin_block1() {
    BlockHeader h;
    h.version = 1;
    h.prev_hash = U("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
    h.merkle_root = U("0e3e2357e806b6cdb1f70b54c3a3a17b6714ee1f0e68bebb44a74b1efd512098");
    h.time = 1231469665;
    h.bits = 0x1d00ffff;
    h.nonce = 2573394689;
    return h;
}

}  // namespace

TEST(BlockHeader, SerializesLikeBitcoin) {
    EXPECT_EQ(to_hex(bitcoin_genesis().serialize()),
              "01000000"                                                          // version
              "0000000000000000000000000000000000000000000000000000000000000000"  // prev_hash
              "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"  // merkle_root, raw bytes
              "29ab5f49"                                                          // time
              "ffff001d"                                                          // bits
              "1dac2b7c");                                                        // nonce
}

TEST(BlockHeader, HashesBitcoinGenesis) {
    EXPECT_EQ(bitcoin_genesis().hash().to_hex(), "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
}

TEST(BlockHeader, HashesBitcoinBlock1) {
    EXPECT_EQ(bitcoin_block1().hash().to_hex(), "00000000839a8e6886ab5951d76f411475428afc90947ee320161bbf18eb6048");
    EXPECT_EQ(bitcoin_block1().prev_hash, bitcoin_genesis().hash());
}

TEST(BlockHeader, BitcoinHeadersMeetTheirTargets) {
    const uint256 limit = U("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    for (const BlockHeader& h : {bitcoin_genesis(), bitcoin_block1()}) {
        EXPECT_TRUE(check_proof_of_work(h.hash(), h.bits, limit));
    }
}

TEST(BlockHeader, DeserializeInvertsSerialize) {
    const auto bytes = bitcoin_block1().serialize();
    EXPECT_EQ(BlockHeader::deserialize(bytes), bitcoin_block1());
}

TEST(BlockHeader, DeserializeParsesFields) {
    const Bytes raw = from_hex(
                          "01000000"
                          "0000000000000000000000000000000000000000000000000000000000000000"
                          "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
                          "29ab5f49ffff001d1dac2b7c")
                          .value();
    EXPECT_EQ(BlockHeader::deserialize(raw), bitcoin_genesis());
}

TEST(BlockHeader, DeserializeNeedsExactly80Bytes) {
    const auto bytes = bitcoin_genesis().serialize();
    EXPECT_THROW(BlockHeader::deserialize(ByteSpan(bytes).first(79)), DeserializeError);

    Bytes longer(bytes.begin(), bytes.end());
    longer.push_back(0);
    EXPECT_THROW(BlockHeader::deserialize(longer), DeserializeError);
}
