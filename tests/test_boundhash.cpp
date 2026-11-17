// Milestone 16: BoundHash, BOUND's proof-of-work hash (docs/boundhash.md).

#include "core/boundhash.h"
#include "core/miner.h"
#include "core/params.h"
#include "core/pow.h"
#include "crypto/sha256.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace bound;

namespace {

// The headers contrib/boundhash/boundhash.py's --vectors hashes.
BlockHeader vector_header(std::uint32_t n) {
    BlockHeader h;
    h.version = 1;
    h.prev_hash = uint256(sha256(bytes_of(std::format("prev {}", n))));
    h.merkle_root = uint256(sha256(bytes_of(std::format("merkle {}", n))));
    h.time = 1'800'000'000 + n;
    h.bits = 0x1e03ffff;
    h.nonce = n;
    return h;
}

struct Vector {
    std::uint32_t lines;
    std::uint32_t n;
    const char* hash;
};

// From the Python reference implementation (python3 contrib/boundhash/boundhash.py --vectors),
// which shares no code with src/core/boundhash.cpp.
constexpr Vector kVectors[] = {
    {2, 0, "add6b5b081123141aaf73e33de7c2a232dcb0269966fe043c7c66466e2887ec1"},
    {2, 1, "4daa442458aeb19fc2b101f960c5767d8a6acc61073a46895335b60864a2d9f4"},
    {2, 2, "2b8f5bbbc16f465386a162e4873396f5c04c1ca150a5a8173aefa566919e5d27"},
    {64, 0, "a1da4b56c502232301c7e9b6d05a2a5eb2d14fe2cab122bec8322f662bf05893"},
    {64, 1, "afd16ab5279a240128c41b726f2345e62606855cd39b543ea5328351c3b14eef"},
    {64, 2, "8237b3e10288bb3b85b040ce43360ae68057eb88d1895ed21515cedccd8987ef"},
    {4096, 0, "a1bfe39eba36286133775ee3db0040b7d638827743a25e29a9eb95ab7e42676b"},
    {4096, 1, "b39b4468daa307a37e9758cb2eca618ff93bfde326420c9d4aad728ae8b435de"},
    {4096, 2, "da06793503671e059bf562f81ee8f1e5286bdcf06b0922c8b6365270df29d5df"},
    {16384, 0, "3a78ae5edb5ed9a3d07d2d63460be9a943dbfc287d7a5f1b67d2f5a4592babe2"},
    {16384, 1, "ba513fcd3c8c42024b36ac4282f6a924afde442831d0bf3c70af8d52137e36a6"},
    {16384, 2, "770c5ba0f1af3fdeb5eb0a3f15625a5e1283a9001cc72634712a7b038b4d9d62"},
};

}  // namespace

TEST(BoundHash, MatchesTheReferenceImplementation) {
    for (const Vector& v : kVectors) {
        EXPECT_EQ(bound_hash(vector_header(v.n), v.lines).to_hex(), v.hash) << v.lines << " lines, header " << v.n;
    }
}

TEST(BoundHash, MainnetAndTestnetUseHalfAMebibyte) {
    EXPECT_EQ(kBoundHashLines * kBoundHashLineBytes, std::size_t{512} << 10);
    EXPECT_EQ(mainnet_params().pow_lines, kBoundHashLines);
    EXPECT_EQ(testnet_params().pow_lines, kBoundHashLines);
    EXPECT_LT(regtest_params().pow_lines, kBoundHashLines);
    EXPECT_TRUE(valid_bound_hash_lines(regtest_params().pow_lines));
}

// One scratchpad per thread, resized as needed: hashing other sizes in between, or on other threads
// at the same time, changes nothing.
TEST(BoundHash, SameAnswerOnAnyThreadAndAfterOtherSizes) {
    const std::string big = kVectors[9].hash;
    const std::string small = kVectors[3].hash;
    EXPECT_EQ(bound_hash(vector_header(0), 64).to_hex(), small);
    EXPECT_EQ(bound_hash(vector_header(0), 16384).to_hex(), big);
    EXPECT_EQ(bound_hash(vector_header(0), 64).to_hex(), small);  // a scratchpad bigger than needed
    EXPECT_EQ(bound_hash(vector_header(0), 16384).to_hex(), big);  // and its leftovers ignored

    std::vector<std::string> results(4);
    {
        std::vector<std::jthread> threads;
        for (std::size_t t = 0; t < results.size(); ++t) {
            threads.emplace_back([&results, t] { results[t] = bound_hash(vector_header(0), t % 2 ? 64 : 16384).to_hex(); });
        }
    }
    for (std::size_t t = 0; t < results.size(); ++t) EXPECT_EQ(results[t], t % 2 ? small : big);
}

// Every bit of the header changes the hash (nonce, time, bits, parent, merkle root, version).
TEST(BoundHash, EveryHeaderBitMatters) {
    const auto base = vector_header(7).serialize();
    std::set<uint256> seen{bound_hash(ByteSpan(base), 64)};
    for (std::size_t bit = 0; bit < base.size() * 8; ++bit) {
        auto flipped = base;
        flipped[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
        ASSERT_TRUE(seen.insert(bound_hash(ByteSpan(flipped), 64)).second) << "bit " << bit;
    }
}

TEST(BoundHash, RefusesBadSizes) {
    const BlockHeader h = vector_header(0);
    for (const std::uint32_t lines : {0u, 1u, 3u, 96u, (1u << 20) + 1, 1u << 21}) {
        EXPECT_FALSE(valid_bound_hash_lines(lines)) << lines;
        EXPECT_THROW(bound_hash(h, lines), std::invalid_argument) << lines;
    }
    const auto bytes = h.serialize();
    EXPECT_THROW(bound_hash(ByteSpan(bytes.data(), 79), 64), std::invalid_argument);
}

// The 128-bit multiply the hash uses agrees with its portable form on awkward values and many
// random ones (the fast form differs per compiler; the portable one is what any port can use).
TEST(BoundHash, MultiplyAgreesWithThePortableForm) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> cases = {
        {0, 0}, {0, kMax}, {1, kMax}, {kMax, kMax}, {kMax, 2}, {0xffffffff, 0xffffffff}, {1ULL << 32, 1ULL << 32},
        {0x123456789abcdef0ULL, 0x0fedcba987654321ULL}};
    std::mt19937_64 rng(16);
    for (int i = 0; i < 10'000; ++i) cases.emplace_back(rng(), rng());
    for (const auto& [a, b] : cases) {
        std::uint64_t hi = 0, lo = 0, phi = 0, plo = 0;
        detail::mul128(a, b, hi, lo);
        detail::mul128_portable(a, b, phi, plo);
        ASSERT_EQ(hi, phi) << a << " * " << b;
        ASSERT_EQ(lo, plo) << a << " * " << b;
    }
    std::uint64_t hi = 0, lo = 0;
    detail::mul128(kMax, kMax, hi, lo);  // (2^64 - 1)^2 = 2^128 - 2^65 + 1
    EXPECT_EQ(hi, kMax - 1);
    EXPECT_EQ(lo, 1u);
}

// Proof of work is BoundHash now, not the block id (SHA-256d): a header can meet its target by one
// and not the other.
TEST(BoundHash, IsWhatProofOfWorkChecks) {
    ConsensusParams p = easy_params();
    p.pow_limit_bits = 0x2000ffff;  // about 1 hash in 256 meets it
    p.pow_limit = target_from_compact(p.pow_limit_bits).value();
    BlockHeader h = vector_header(1);
    h.bits = p.pow_limit_bits;
    const uint256 target = target_from_compact(h.bits).value();

    bool found_by_pow_only = false;
    bool found_by_id_only = false;
    for (h.nonce = 0; h.nonce < 20'000 && !(found_by_pow_only && found_by_id_only); ++h.nonce) {
        const bool by_pow = bound_hash(h, p.pow_lines) <= target;
        const bool by_id = h.hash() <= target;
        EXPECT_EQ(check_proof_of_work(h, p), by_pow);
        EXPECT_EQ(pow_hash(h, p), bound_hash(h, p.pow_lines));
        if (by_pow && !by_id) found_by_pow_only = true;
        if (by_id && !by_pow) found_by_id_only = true;
    }
    EXPECT_TRUE(found_by_pow_only);
    EXPECT_TRUE(found_by_id_only);

    // The miner finds the first nonce BoundHash accepts.
    h.nonce = 0;
    const std::optional<BlockHeader> mined = mine(h, p);
    ASSERT_TRUE(mined.has_value());
    for (std::uint32_t n = 0; n < mined->nonce; ++n) {
        BlockHeader earlier = h;
        earlier.nonce = n;
        ASSERT_FALSE(check_proof_of_work(earlier, p)) << n;
    }
    EXPECT_TRUE(check_proof_of_work(*mined, p));
}

TEST(BoundHash, RefusesBitsOutsideTheLimit) {
    const ConsensusParams& p = regtest_params();
    BlockHeader h = vector_header(0);
    for (const std::uint32_t bits : {0x00000000u, 0x2100ffffu, 0x20800000u /* negative */}) {
        h.bits = bits;
        EXPECT_FALSE(check_proof_of_work(h, p)) << std::hex << bits;
    }
}

// How fast this machine hashes on one thread at mainnet's size. Not run by default:
//   bound_tests --gtest_also_run_disabled_tests --gtest_filter=BoundHash.DISABLED_Speed
TEST(BoundHash, DISABLED_Speed) {
    const BlockHeader h = vector_header(0);
    auto bytes = h.serialize();
    (void)bound_hash(ByteSpan(bytes), kBoundHashLines);  // the scratchpad's first allocation
    const auto start = std::chrono::steady_clock::now();
    int count = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        bytes[76] = static_cast<std::uint8_t>(count++);
        (void)bound_hash(ByteSpan(bytes), kBoundHashLines);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("BoundHash: %.0f hashes a second on one thread (%.3f ms each)\n", count / seconds, 1000 * seconds / count);
}
