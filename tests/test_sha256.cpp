#include "crypto/sha256.h"
#include "util/hex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

using namespace bound;

namespace {

// 0, 1, 2, ..., 250, 0, 1, ...
Bytes pattern(std::size_t n) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(i % 251);
    return b;
}

}  // namespace

TEST(Sha256, NistVectors) {
    EXPECT_EQ(to_hex(sha256(bytes_of(""))),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(to_hex(sha256(bytes_of("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(to_hex(sha256(bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(to_hex(sha256(bytes_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                     "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256, OneMillionAs) {
    const std::string a(1'000'000, 'a');
    EXPECT_EQ(to_hex(sha256(bytes_of(a))),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Lengths around the 55/56-byte edge (where the 8-byte length no longer fits in the last block)
// and the 64-byte block edge.
TEST(Sha256, PaddingEdges) {
    struct Case {
        std::size_t len;
        const char* hex;
    };
    const Case cases[] = {
        {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {119, "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6"},
        {120, "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c"},
        {128, "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
    };
    for (const Case& c : cases) EXPECT_EQ(to_hex(sha256(pattern(c.len))), c.hex) << "len=" << c.len;
}

TEST(Sha256, IncrementalMatchesOneShot) {
    const Bytes data = pattern(1000);
    const std::string expected = to_hex(sha256(data));
    const std::size_t chunks[] = {1, 3, 55, 56, 63, 64, 65, 999, 1000};
    for (std::size_t chunk : chunks) {
        Sha256 h;
        for (std::size_t i = 0; i < data.size(); i += chunk)
            h.write(ByteSpan(data).subspan(i, std::min(chunk, data.size() - i)));
        EXPECT_EQ(to_hex(h.finalize()), expected) << "chunk=" << chunk;
    }
}

TEST(Sha256, EmptyWritesAreNoOps) {
    Sha256 h;
    h.write(ByteSpan{}).write(bytes_of("abc")).write(ByteSpan{});
    EXPECT_EQ(to_hex(h.finalize()), to_hex(sha256(bytes_of("abc"))));
}

TEST(Sha256, ResetStartsOver) {
    Sha256 h;
    h.write(bytes_of("garbage"));
    h.reset();
    h.write(bytes_of("abc"));
    EXPECT_EQ(to_hex(h.finalize()), to_hex(sha256(bytes_of("abc"))));
}

// The miner's midstate trick depends on this: hash a shared prefix once, copy, finish each copy.
TEST(Sha256, CopyCarriesTheState) {
    Sha256 prefix;
    prefix.write(pattern(100));
    Sha256 copy = prefix;
    prefix.write(bytes_of("tail"));
    copy.write(bytes_of("tail"));
    const std::string a = to_hex(prefix.finalize());
    EXPECT_EQ(a, to_hex(copy.finalize()));

    Bytes whole = pattern(100);
    const ByteSpan tail = bytes_of("tail");
    whole.insert(whole.end(), tail.begin(), tail.end());
    EXPECT_EQ(a, to_hex(sha256(whole)));
}

TEST(Sha256, DoubleSha256) {
    EXPECT_EQ(to_hex(sha256d(bytes_of(""))),
              "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456");
    EXPECT_EQ(to_hex(sha256d(bytes_of("abc"))),
              "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358");
}

TEST(Sha256, TaggedHash) {
    EXPECT_EQ(to_hex(tagged_hash("BOUND/test", bytes_of("abc"))),
              "bc08becd88ba8d0b923f546cac77434384b6033823e21144a09ad90f4e69a34c");
    // Same data under different tags must hash differently.
    EXPECT_NE(to_hex(tagged_hash("BOUND/a", bytes_of("abc"))), to_hex(tagged_hash("BOUND/b", bytes_of("abc"))));
}
