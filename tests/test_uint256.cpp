#include "core/uint256.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <unordered_set>

using namespace bound;

namespace {

constexpr const char* kBitcoinGenesis = "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f";
constexpr const char* kBitcoinBlock1 = "00000000839a8e6886ab5951d76f411475428afc90947ee320161bbf18eb6048";

}  // namespace

TEST(Uint256, DefaultIsZero) {
    const uint256 z;
    EXPECT_TRUE(z.is_zero());
    EXPECT_EQ(z.to_hex(), std::string(64, '0'));
    EXPECT_FALSE(U(kBitcoinGenesis).is_zero());
}

TEST(Uint256, HexIsDisplayOrder) {
    const uint256 h = U(kBitcoinGenesis);
    EXPECT_EQ(h.bytes()[0], 0x6f);   // least significant byte is stored first...
    EXPECT_EQ(h.bytes()[31], 0x00);  // ...and printed last
    EXPECT_EQ(h.to_hex(), kBitcoinGenesis);
}

TEST(Uint256, FromHexValidates) {
    EXPECT_FALSE(uint256::from_hex("").has_value());
    EXPECT_FALSE(uint256::from_hex(std::string(63, '0')).has_value());
    EXPECT_FALSE(uint256::from_hex(std::string(65, '0')).has_value());
    EXPECT_FALSE(uint256::from_hex(std::string(63, '0') + "g").has_value());

    const auto upper = uint256::from_hex("000000000019D6689C085AE165831E934FF763AE46A2A6C172B3F1B60A8CE26F");
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(*upper, U(kBitcoinGenesis));
}

TEST(Uint256, ComparesNumerically) {
    Digest32 high{};
    high[31] = 0x01;  // 2^248
    Digest32 low{};
    low[0] = 0xff;  // 255
    EXPECT_LT(uint256(low), uint256(high));
    EXPECT_GT(uint256(high), uint256(low));
    EXPECT_LE(uint256(low), uint256(low));
    EXPECT_EQ(uint256(low), uint256(low));
    EXPECT_NE(uint256(low), uint256(high));
    EXPECT_LT(U(kBitcoinGenesis), U(kBitcoinBlock1));  // more leading zeros: smaller number
}

TEST(Uint256, WorksAsHashKey) {
    const std::hash<uint256> hasher;
    EXPECT_NE(hasher(U(kBitcoinGenesis)), hasher(U(kBitcoinBlock1)));

    std::unordered_set<uint256> set;
    set.insert(U(kBitcoinGenesis));
    set.insert(U(kBitcoinBlock1));
    set.insert(U(kBitcoinGenesis));
    EXPECT_EQ(set.size(), 2u);
    EXPECT_TRUE(set.contains(U(kBitcoinBlock1)));
}
