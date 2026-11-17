#include "core/boundhash.h"

#include "crypto/sha256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
#endif

namespace bound {

namespace detail {

void mul128_portable(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) {
    constexpr std::uint64_t kLow = 0xffffffff;
    const std::uint64_t a0 = a & kLow, a1 = a >> 32, b0 = b & kLow, b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const std::uint64_t middle = (p00 >> 32) + (p01 & kLow) + (p10 & kLow);  // < 3 * 2^32: no overflow
    lo = (middle << 32) | (p00 & kLow);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}

void mul128(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) {
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    hi = static_cast<std::uint64_t>(product >> 64);
    lo = static_cast<std::uint64_t>(product);
#elif defined(_MSC_VER) && defined(_M_X64)
    lo = _umul128(a, b, &hi);
#elif defined(_MSC_VER) && defined(_M_ARM64)
    lo = a * b;
    hi = __umulh(a, b);
#else
    mul128_portable(a, b, hi, lo);
#endif
}

}  // namespace detail

namespace {

constexpr std::size_t kWords = kBoundHashLineBytes / 8;  // 16
static_assert(kWords == 16);

struct alignas(64) Line {
    std::uint64_t w[kWords];
};

std::uint64_t load_le64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

void store_le64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// The round works on 16 named locals, v0 .. v15, not an array: compilers keep those in registers,
// where MSVC kept an array in memory and ran at half the speed. Hence macros, undefined below.
//
// Argon2's BlaMka: an addition with a multiplication of the low halves folded in, which diffuses
// faster than a plain addition and costs a CPU one cheap multiply.
#define BOUND_BLAMKA(x, y) \
    ((x) + (y) + 2 * (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * static_cast<std::uint32_t>(y)))
// BLAKE2b's G without message words, with BlaMka for its additions: Argon2's.
#define BOUND_G(a, b, c, d)       \
    do {                          \
        a = BOUND_BLAMKA(a, b);   \
        d = std::rotr(d ^ a, 32); \
        c = BOUND_BLAMKA(c, d);   \
        b = std::rotr(b ^ c, 24); \
        a = BOUND_BLAMKA(a, b);   \
        d = std::rotr(d ^ a, 16); \
        c = BOUND_BLAMKA(c, d);   \
        b = std::rotr(b ^ c, 63); \
    } while (false)
// One round on the 16 words: the columns, then the diagonals. After it every word depends on all 16.
#define BOUND_ROUND()              \
    do {                           \
        BOUND_G(v0, v4, v8, v12);  \
        BOUND_G(v1, v5, v9, v13);  \
        BOUND_G(v2, v6, v10, v14); \
        BOUND_G(v3, v7, v11, v15); \
        BOUND_G(v0, v5, v10, v15); \
        BOUND_G(v1, v6, v11, v12); \
        BOUND_G(v2, v7, v8, v13);  \
        BOUND_G(v3, v4, v9, v14);  \
    } while (false)
#define BOUND_LOAD(src)                                                                                       \
    std::uint64_t v0 = (src)[0], v1 = (src)[1], v2 = (src)[2], v3 = (src)[3], v4 = (src)[4], v5 = (src)[5],   \
                  v6 = (src)[6], v7 = (src)[7], v8 = (src)[8], v9 = (src)[9], v10 = (src)[10], v11 = (src)[11], \
                  v12 = (src)[12], v13 = (src)[13], v14 = (src)[14], v15 = (src)[15]

// A scratchpad line from x, with its position i as a tweak: one round, then x added back in (so the
// step can't be run backwards, as in Argon2's compression).
inline void fill_line(const std::uint64_t* x, std::uint64_t i, std::uint64_t* out) {
    BOUND_LOAD(x);
    v15 ^= i;
    BOUND_ROUND();
    out[0] = v0 ^ x[0], out[1] = v1 ^ x[1], out[2] = v2 ^ x[2], out[3] = v3 ^ x[3];
    out[4] = v4 ^ x[4], out[5] = v5 ^ x[5], out[6] = v6 ^ x[6], out[7] = v7 ^ x[7];
    out[8] = v8 ^ x[8], out[9] = v9 ^ x[9], out[10] = v10 ^ x[10], out[11] = v11 ^ x[11];
    out[12] = v12 ^ x[12], out[13] = v13 ^ x[13], out[14] = v14 ^ x[14], out[15] = v15 ^ x[15];
}

}  // namespace

uint256 bound_hash(ByteSpan header, std::uint32_t lines) {
    if (!valid_bound_hash_lines(lines)) throw std::invalid_argument("BoundHash: lines must be a power of two from 2 to 2^20");
    if (header.size() != BlockHeader::kSize) throw std::invalid_argument("BoundHash: a header is 80 bytes");

    thread_local std::vector<Line> scratchpad;
    if (scratchpad.size() < lines) scratchpad.resize(lines);
    Line* mem = scratchpad.data();

    // 1. The seed, and from it the starting state: 16 words from four SHA-256s of it.
    const Digest32 seed = tagged_hash("BOUND/BoundHash", header);
    std::uint64_t start[kWords];
    for (std::uint8_t k = 0; k < 4; ++k) {
        const Digest32 d = Sha256().write(seed).write(ByteSpan(&k, 1)).finalize();
        for (std::size_t q = 0; q < 4; ++q) start[4 * k + q] = load_le64(d.data() + 8 * q);
    }

    // 2. Fill: line i comes from line i - 1 and, from line 2 on, an earlier line the data picks,
    // anywhere in 0 .. i - 2 (never i - 1 itself, which would cancel out).
    std::uint64_t x[kWords];
    fill_line(start, 0, mem[0].w);
    fill_line(mem[0].w, 1, mem[1].w);
    for (std::uint32_t i = 2; i < lines; ++i) {
        const std::uint64_t* prev = mem[i - 1].w;
        const auto ref = static_cast<std::uint32_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(prev[0])) * (i - 1)) >> 32);
        const std::uint64_t* other = mem[ref].w;
        for (std::size_t k = 0; k < kWords; ++k) x[k] = prev[k] ^ other[k];
        fill_line(x, i, mem[i].w);
    }

    // 3. Mix: `lines` steps, each reading the line the state points at, multiplying, running a
    // round, and adding the new state into that line. Every step waits for the one before it.
    for (std::size_t k = 0; k < kWords; ++k) x[k] = start[k] ^ mem[lines - 1].w[k];
    BOUND_LOAD(x);
    for (std::uint64_t r = 0; r < lines; ++r) {
        std::uint64_t* l = mem[v0 & (lines - 1)].w;
        v0 ^= l[0], v1 ^= l[1], v2 ^= l[2], v3 ^= l[3], v4 ^= l[4], v5 ^= l[5], v6 ^= l[6], v7 ^= l[7];
        v8 ^= l[8], v9 ^= l[9], v10 ^= l[10], v11 ^= l[11], v12 ^= l[12], v13 ^= l[13], v14 ^= l[14], v15 ^= l[15];
        std::uint64_t hi = 0;
        std::uint64_t lo = 0;
        detail::mul128(v0, v1 | 1, hi, lo);
        v2 += hi;
        v3 ^= lo;
        v15 ^= r;
        BOUND_ROUND();
        l[0] += v0, l[1] += v1, l[2] += v2, l[3] += v3, l[4] += v4, l[5] += v5, l[6] += v6, l[7] += v7;
        l[8] += v8, l[9] += v9, l[10] += v10, l[11] += v11, l[12] += v12, l[13] += v13, l[14] += v14, l[15] += v15;
    }

    // 4. The result: SHA-256 (tagged) of the seed and the final state.
    const std::uint64_t state[kWords] = {v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15};
    std::array<std::uint8_t, 32 + kBoundHashLineBytes> out{};
    std::copy(seed.begin(), seed.end(), out.begin());
    for (std::size_t k = 0; k < kWords; ++k) store_le64(out.data() + 32 + 8 * k, state[k]);
    return uint256(tagged_hash("BOUND/BoundHash-result", out));
}

#undef BOUND_LOAD
#undef BOUND_ROUND
#undef BOUND_G
#undef BOUND_BLAMKA

}  // namespace bound
