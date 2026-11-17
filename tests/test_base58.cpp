#include "util/base58.h"
#include "util/hex.h"

#include <gtest/gtest.h>

#include <string_view>

using namespace bound;

namespace {

struct Vector {
    const char* hex;
    const char* base58;
};

// Bitcoin Core's src/test/data/base58_encode_decode.json.
constexpr Vector kCoreVectors[] = {
    {"", ""},
    {"61", "2g"},
    {"626262", "a3gV"},
    {"636363", "aPEr"},
    {"73696d706c792061206c6f6e6720737472696e67", "2cFupjhnEsSn59qHXstmK2ffpLv2"},
    {"00eb15231dfceb60925886b67d065299925915aeb172c06647", "1NS17iag9jJgTHD1VXjvLCEnZuQ3rJDE9L"},
    {"516b6fcd0f", "ABnLTmg"},
    {"bf4f89001e670274dd", "3SEo3LWLoPntC"},
    {"572e4794", "3EFU7m"},
    {"ecac89cad93923c02321", "EJDM8drfXA6uyA"},
    {"10c8511e", "Rt5zm"},
    {"00000000000000000000", "1111111111"},
};

Bytes unhex(std::string_view hex) { return from_hex(hex).value(); }

}  // namespace

TEST(Base58, EncodesBitcoinCoreVectors) {
    for (const Vector& v : kCoreVectors) EXPECT_EQ(base58_encode(unhex(v.hex)), v.base58) << "hex=" << v.hex;
}

TEST(Base58, DecodesBitcoinCoreVectors) {
    for (const Vector& v : kCoreVectors) EXPECT_EQ(base58_decode(v.base58), unhex(v.hex)) << "base58=" << v.base58;
}

TEST(Base58, RejectsCharactersOutsideTheAlphabet) {
    for (const char* s : {"0", "O", "I", "l", "+", "3SEo3LWL oPntC", " 2g", "2g\n"}) {
        EXPECT_FALSE(base58_decode(s).has_value()) << "input=\"" << s << "\"";
    }
}

TEST(Base58Check, ReproducesSatoshisAddress) {
    // Version 0x00 + HASH160 of the public key in Bitcoin's genesis coinbase.
    const Bytes payload = unhex("0062e907b15cbf27d5425399ebf6f0fb50ebb88f18");
    EXPECT_EQ(base58check_encode(payload), "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa");
    EXPECT_EQ(base58check_decode("1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa"), payload);
}

TEST(Base58Check, RejectsBadChecksum) {
    EXPECT_FALSE(base58check_decode("1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNb").has_value());  // last char changed
}

TEST(Base58Check, RejectsInputsTooShortForAChecksum) {
    EXPECT_FALSE(base58check_decode("").has_value());
    EXPECT_FALSE(base58check_decode(base58_encode(unhex("010203"))).has_value());
}

TEST(Base58Check, EmptyPayloadRoundTrips) {
    EXPECT_EQ(base58check_decode(base58check_encode(ByteSpan{})), Bytes{});
}
