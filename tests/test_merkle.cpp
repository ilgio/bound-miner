#include "core/merkle.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace bound;

namespace {

std::vector<uint256> leaves(int n) {
    std::vector<uint256> out;
    for (int i = 0; i < n; ++i) out.push_back(test_hash("leaf " + std::to_string(i)));
    return out;
}

}  // namespace

TEST(Merkle, NoLeavesIsZero) {
    EXPECT_TRUE(merkle_root({}).is_zero());
}

TEST(Merkle, OneLeafIsTheRoot) {
    const std::vector<uint256> one = leaves(1);
    EXPECT_EQ(merkle_root(one), one[0]);
}

TEST(Merkle, KnownRoots) {
    const char* expected[] = {
        "a844f17709054df68211f6a2792083f6674fb56be4b5af8e9152267ec3021bf8",  // 1 leaf
        "5f6c08c24f1c7dbe4400f23af44b52bd205d4a0b3b9feed3210bb25d8b772016",  // 2
        "5ae11731d472f8ecba5c8f13ae301e37d520c6da3dd0ff86a89efb261cc8107f",  // 3
        "17ce03b46bec101f3a85e477741743051e97b7905356ef5633e3ad6e442b68e6",  // 4
        "d6c57260fcfbebadae957b35373ac02c44940846f49b9fe94832ac419df727aa",  // 5
        "2e35cde58e9eb0758c99ef0946cca666e7711bac5f275a7152cd8706aa5d9a0c",  // 6
        "5c57d37872e0a0251f31de233ab92881ea2185cba0f77bb60dd4cb2da99e977e",  // 7
    };
    for (int n = 1; n <= 7; ++n) EXPECT_EQ(merkle_root(leaves(n)).to_hex(), expected[n - 1]) << n << " leaves";
}

// The CVE-2012-2459 shape: with duplication, these two lists would share a root.
TEST(Merkle, OddNodeIsPromotedNotDuplicated) {
    const std::vector<uint256> three = leaves(3);
    std::vector<uint256> four = three;
    four.push_back(three[2]);
    EXPECT_NE(merkle_root(three), merkle_root(four));
}

// Promotion means an inner node handed in as a leaf gives the same root. That's harmless only
// because real leaves (wtxids) are tagged differently from inner nodes, so no transaction can
// pose as an inner node.
TEST(Merkle, PromotionStructure) {
    const std::vector<uint256> three = leaves(3);
    const std::vector<uint256> collapsed = {merkle_root(std::span(three).first(2)), three[2]};
    EXPECT_EQ(merkle_root(three), merkle_root(collapsed));
}

TEST(Merkle, OrderMatters) {
    std::vector<uint256> l = leaves(4);
    const uint256 root = merkle_root(l);
    std::swap(l[1], l[2]);
    EXPECT_NE(merkle_root(l), root);
}
