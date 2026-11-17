#include "crypto/ed25519.h"
#include "util/hex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace bound;

namespace {

template <std::size_t N>
std::array<std::uint8_t, N> arr(std::string_view hex) {
    const Bytes b = from_hex(hex).value();
    if (b.size() != N) throw std::invalid_argument("test vector has the wrong length");
    std::array<std::uint8_t, N> out{};
    std::copy(b.begin(), b.end(), out.begin());
    return out;
}

struct RfcVector {
    const char* seed;
    const char* pk;
    const char* msg;
    const char* sig;
};

// RFC 8032 section 7.1, TEST 1-3.
constexpr RfcVector kRfc[] = {
    {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
};

}  // namespace

TEST(Ed25519, Rfc8032Vectors) {
    for (const RfcVector& v : kRfc) {
        const KeyPair k = KeyPair::from_seed(arr<32>(v.seed));
        EXPECT_EQ(to_hex(k.public_key()), v.pk);
        const Bytes msg = from_hex(v.msg).value();
        const Signature sig = k.sign(msg);
        EXPECT_EQ(to_hex(sig), v.sig);
        EXPECT_TRUE(verify(k.public_key(), msg, sig));
    }
}

TEST(Ed25519, RejectsTampering) {
    const KeyPair k = KeyPair::from_seed(arr<32>(kRfc[2].seed));
    const Bytes msg = from_hex(kRfc[2].msg).value();
    const Signature sig = k.sign(msg);

    Bytes other_msg = msg;
    other_msg[0] ^= 0x01;
    EXPECT_FALSE(verify(k.public_key(), other_msg, sig));

    Signature other_sig = sig;
    other_sig[10] ^= 0x01;
    EXPECT_FALSE(verify(k.public_key(), msg, other_sig));

    const KeyPair other_key = KeyPair::from_seed(arr<32>(kRfc[0].seed));
    EXPECT_FALSE(verify(other_key.public_key(), msg, sig));
}

// RFC 8032 requires S < L. TEST 1's signature with L added to S still passes a lax verifier for
// the same message: a second encoding of one signature, i.e. malleability. Consensus rejects it.
TEST(Ed25519, RejectsNonCanonicalS) {
    const PublicKey pk = arr<32>(kRfc[0].pk);
    EXPECT_TRUE(verify(pk, {}, arr<64>(kRfc[0].sig)));
    EXPECT_FALSE(verify(pk, {}, arr<64>("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                                        "4c8c7872aa064e049dbb3013fbf29380d25bf5f0595bbe24655141438e7a101b")));
}

// The identity point (encoded 01 00 .. 00) has small order. A verifier without a small-order
// check accepts (R = base point, S = 1) under it for *every* message: coins anyone can spend.
TEST(Ed25519, RejectsSmallOrderPublicKey) {
    const PublicKey identity = arr<32>("0100000000000000000000000000000000000000000000000000000000000000");
    const Signature forged = arr<64>("5866666666666666666666666666666666666666666666666666666666666666"
                                     "0100000000000000000000000000000000000000000000000000000000000000");
    EXPECT_FALSE(verify(identity, bytes_of("pay attacker"), forged));
}

TEST(Ed25519, GeneratedKeysAreFreshAndWork) {
    const KeyPair a = KeyPair::generate();
    const KeyPair b = KeyPair::generate();
    EXPECT_NE(to_hex(a.public_key()), to_hex(b.public_key()));

    const Signature sig = a.sign(bytes_of("hello"));
    EXPECT_TRUE(verify(a.public_key(), bytes_of("hello"), sig));
    EXPECT_FALSE(verify(b.public_key(), bytes_of("hello"), sig));
}

TEST(Ed25519, KeyPairIsMoveOnly) {
    static_assert(!std::is_copy_constructible_v<KeyPair>);
    static_assert(!std::is_copy_assignable_v<KeyPair>);
    static_assert(std::is_nothrow_move_constructible_v<KeyPair>);
    static_assert(std::is_nothrow_move_assignable_v<KeyPair>);

    KeyPair a = KeyPair::from_seed(arr<32>(kRfc[0].seed));
    KeyPair b = std::move(a);
    EXPECT_EQ(to_hex(b.public_key()), kRfc[0].pk);
    EXPECT_EQ(to_hex(b.sign({})), kRfc[0].sig);

    KeyPair c = KeyPair::generate();
    c = std::move(b);
    EXPECT_EQ(to_hex(c.public_key()), kRfc[0].pk);
    EXPECT_EQ(to_hex(c.sign({})), kRfc[0].sig);
}
