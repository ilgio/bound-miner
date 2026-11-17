#include "core/address.h"
#include "test_util.h"
#include "util/hex.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace bound;

namespace {

constexpr std::uint8_t kTestnet = 0xf0;
constexpr const char* kAliceAddress = "959oE3KAfssVu6WySPxjw6hFYWBHG5Qqjrtq4VVwuRnpcJHBEro";

}  // namespace

TEST(Address, EncodesAKnownKey) {
    EXPECT_EQ(encode_address(test_key("alice").public_key(), kTestnet), kAliceAddress);
}

TEST(Address, TestnetAddressesStartWith9) {
    for (const char* name : {"alice", "bob", "carol"}) {
        const std::string a = encode_address(test_key(name).public_key(), kTestnet);
        EXPECT_EQ(a.size(), 51u) << name;
        EXPECT_EQ(a[0], '9') << name;
    }
}

TEST(Address, RoundTrips) {
    const PublicKey pk = test_key("bob").public_key();
    const std::optional<PublicKey> back = decode_address(encode_address(pk, kTestnet), kTestnet);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(to_hex(*back), to_hex(pk));
}

TEST(Address, RejectsOtherVersions) {
    // Alice's key under version 0xf1.
    const char* other = "976YQGmCXJYj2v6gLyajtRJ2TcnzunFiXVxwJeBqu5oD6swBvEo";
    EXPECT_FALSE(decode_address(other, kTestnet).has_value());
    EXPECT_TRUE(decode_address(other, 0xf1).has_value());
}

TEST(Address, RejectsWrongPayloadLength) {
    // Valid Base58Check, version 0xf0, but a 31-byte key.
    EXPECT_FALSE(decode_address("2q4oivYJbduDtgK7tyQWDFtG5RHNov7JqgQ56Kh6ByNFp48cWi", kTestnet).has_value());
    // A Bitcoin address: valid Base58Check with version 0x00, but a 20-byte hash.
    EXPECT_FALSE(decode_address("1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa", 0x00).has_value());
}

TEST(Address, RejectsATypo) {
    std::string a = kAliceAddress;
    a[20] = a[20] == 'x' ? 'y' : 'x';
    EXPECT_FALSE(decode_address(a, kTestnet).has_value());
}

TEST(Address, RejectsOverlongInput) {
    EXPECT_FALSE(decode_address(std::string(65, '9'), kTestnet).has_value());
    EXPECT_FALSE(decode_address(std::string(100'000, '1'), kTestnet).has_value());  // and returns at once
}
