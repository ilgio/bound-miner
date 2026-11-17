#pragma once

// Milestone 16: BoundHash, BOUND's own proof-of-work hash. The full specification, the reasons for
// each step and a reference implementation (contrib/boundhash/boundhash.py) are in docs/boundhash.md.
//
// In short: SHA-256 turns the 80-byte header into a seed; the seed fills a scratchpad of `lines`
// 128-byte lines (512 KiB on mainnet and testnet), each line mixing the one before it with an
// earlier one that the data picks, through one round of Argon2's mixing (BLAKE2b's round with
// multiplications); then `lines` steps read and rewrite lines at places the data picks, each a
// 64 x 64 -> 128-bit multiply and another round. SHA-256 of the seed and the final state is the
// result. Under a millisecond on one core of a desktop CPU.
//
// Bitcoin's SHA-256 machines (ASICs) can't compute it at all. A machine built for it would need the
// scratchpad's fast memory beside every core, and the multiplications a CPU already has, so it
// gains far less over a PC than SHA-256 machines do. The SHA-256 at both ends keeps it a sound
// proof of work even if the middle had a shortcut: at worst, a shortcut would make that machine
// cheaper to build, never let anyone forge a block.

#include "core/block.h"
#include "core/uint256.h"
#include "util/bytes.h"

#include <cstddef>
#include <cstdint>

namespace bound {

// The scratchpad's size on mainnet and testnet: 4,096 lines of 128 bytes, 512 KiB. Measured on a
// 6-core desktop (docs/boundhash.md), 2 MiB stalled on RAM after 2-4 threads (~600 hashes a second
// in all, ~3 ms to check one) while 512 KiB stays in the caches (~5,400 a second, 0.7 ms to check),
// and every node checks every header, the small servers too. Regtest uses fewer lines so tests stay
// quick (ConsensusParams::pow_lines).
inline constexpr std::uint32_t kBoundHashLines = std::uint32_t{1} << 12;
inline constexpr std::size_t kBoundHashLineBytes = 128;

// A power of two from 2 to 2^20 (128 MiB).
constexpr bool valid_bound_hash_lines(std::uint32_t lines) {
    return lines >= 2 && lines <= (std::uint32_t{1} << 20) && (lines & (lines - 1)) == 0;
}

// BoundHash of an 80-byte serialized header with `lines` lines of scratchpad. The scratchpad is kept
// per thread and reused, so a thread allocates it once. Throws std::invalid_argument unless
// valid_bound_hash_lines(lines) and the header is 80 bytes.
uint256 bound_hash(ByteSpan header, std::uint32_t lines);
inline uint256 bound_hash(const BlockHeader& header, std::uint32_t lines) {
    return bound_hash(ByteSpan(header.serialize()), lines);
}

namespace detail {
// The 64 x 64 -> 128-bit product BoundHash uses, from 32-bit pieces only: what the fast path
// (the compiler's 128-bit integers or _umul128) must agree with. Exposed for tests.
void mul128_portable(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo);
void mul128(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo);
}  // namespace detail

}  // namespace bound
