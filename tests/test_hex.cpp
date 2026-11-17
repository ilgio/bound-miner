#include "util/hex.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace bound;

TEST(Hex, EncodesLowercaseInOrder) {
    EXPECT_EQ(to_hex(Bytes{0x00, 0x01, 0xab, 0xff}), "0001abff");
    EXPECT_EQ(to_hex(Bytes{}), "");
}

TEST(Hex, DecodesEitherCase) {
    EXPECT_EQ(from_hex("0001ABff"), (Bytes{0x00, 0x01, 0xab, 0xff}));
    EXPECT_EQ(from_hex(""), Bytes{});
}

TEST(Hex, RejectsMalformedInput) {
    EXPECT_FALSE(from_hex("abc").has_value());  // odd length
    EXPECT_FALSE(from_hex("zz").has_value());
    EXPECT_FALSE(from_hex("0g").has_value());
    EXPECT_FALSE(from_hex("0x00").has_value());
    EXPECT_FALSE(from_hex("00 1").has_value());
}

TEST(Hex, RoundTripsEveryByteValue) {
    Bytes all(256);
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<std::uint8_t>(i);
    EXPECT_EQ(from_hex(to_hex(all)), all);
}
