#pragma once

#include "core/arith_uint256.h"
#include "core/uint256.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace bound {

struct BlockHeader;
struct BlockIndex;
struct ConsensusParams;

// Decodes the compact "nBits" target encoding: exponent = bits >> 24, mantissa =
// bits & 0x007fffff, value = mantissa * 256^(exponent - 3); 0x00800000 is a sign bit.
// Same semantics as Bitcoin's arith_uint256::SetCompact, with its negative and overflow flags
// folded into nullopt: returns nullopt if the value is negative (sign bit set and value
// non-zero) or doesn't fit in 256 bits. Zero is a valid result.
std::optional<uint256> target_from_compact(std::uint32_t bits);

// Canonical compact encoding of target: keeps only its 3 most significant bytes (so it's
// lossy) and never sets the sign bit. Same as arith_uint256::GetCompact(false).
std::uint32_t compact_from_target(const uint256& target);

// True iff bits decodes to a target t with 0 < t <= pow_limit, and hash <= t.
bool check_proof_of_work(const uint256& hash, std::uint32_t bits, const uint256& pow_limit);

// Milestone 16: the number a header's proof of work is: BoundHash of its 80 bytes, with the
// network's scratchpad size. Not the block's id, which stays header.hash() (SHA-256d) as on
// Litecoin: ids are needed everywhere and must be cheap, proof of work only where blocks enter.
uint256 pow_hash(const BlockHeader& header, const ConsensusParams& params);
// check_proof_of_work(pow_hash(header, params), header.bits, params.pow_limit), except that bits
// outside (0, pow_limit] are turned away before any hashing, as BoundHash costs about a millisecond.
bool check_proof_of_work(const BlockHeader& header, const ConsensusParams& params);

// Milestone 4: work and difficulty adjustment.

// The expected number of hashes it takes to meet bits' target: 2^256 / (target + 1), rounded
// down (Bitcoin's GetBlockProof). A chain's work is this summed over its blocks, and the chain
// with the most work wins. 0 if bits doesn't decode to a positive target.
arith_uint256 block_proof(std::uint32_t bits);

// Bitcoin's retarget formula (CalculateNextWorkRequired): bits' target times actual_timespan /
// expected_timespan, with actual_timespan first clamped to [expected / 4, expected * 4] so one
// retarget changes the difficulty at most 4x either way, and the result capped at pow_limit.
// Preconditions: bits decodes to a positive target, and 0 < expected_timespan <= 2^30 (throws
// std::invalid_argument otherwise), which keeps 4 * expected within 32 bits.
std::uint32_t retarget(std::uint32_t bits, std::int64_t actual_timespan, std::int64_t expected_timespan,
                       const uint256& pow_limit);

// Milestone 11: aserti3-2d, Bitcoin Cash's difficulty rule (the November 2020 specification),
// computing in 16-bit fixed point:
//   target = anchor_target * 2^((time_diff - spacing * (height_diff + 1)) / half_life)
// where time_diff is the parent's time minus the anchor's parent's (the anchor's own when it's the
// genesis), and height_diff is the parent's height minus the anchor's. Clamped to [1, pow_limit].
//
// It gives exactly the specification's result wherever the specification applies (pow_limit below
// 2^224; Bitcoin Cash's test vectors check that), and stays exact for an easier pow_limit, where
// the specification's own arithmetic would overflow: anchor_target * factor is multiplied and
// divided in one exact step (mul_div) instead of in 256 bits.
// Throws std::invalid_argument unless 0 < anchor_target <= pow_limit, height_diff >= 0,
// spacing > 0, half_life > 0 and |time_diff - spacing * height_diff| < 2^47 (as the spec asserts).
arith_uint256 asert_target(const arith_uint256& anchor_target, std::int64_t time_diff, std::int64_t height_diff,
                           std::int64_t spacing, std::int64_t half_life, const arith_uint256& pow_limit);

// The bits a block on top of `parent` must have.
//   - With params.asert_half_life set (mainnet): asert_target, anchored at the genesis block.
//   - Otherwise (testnet): the difficulty only changes at the start of each period of
//     params.retarget_interval blocks (heights that are multiples of it); other blocks copy their
//     parent's bits. A retarget compares how long the last period took, from the last block of
//     the period before it (genesis, for the first one) to `parent`, with target_spacing per block.
//   - With params.pow_no_retargeting (regtest): the parent's bits.
std::uint32_t next_work_required(const BlockIndex& parent, const ConsensusParams& params);
// The same rule from plain values, for headers that aren't in the block tree (headers presync).
// time_at(h) gives the time of the parent's ancestor at height h; it's only called when a
// retarget is due, for h = max(0, parent_height - retarget_interval).
std::uint32_t next_work_required(int parent_height, std::uint32_t parent_bits, std::int64_t parent_time,
                                 const std::function<std::int64_t(int)>& time_at, const ConsensusParams& params);

// How many times harder bits' target is than params.pow_limit: 1.0 at the limit. For display.
double difficulty(std::uint32_t bits, const ConsensusParams& params);

}  // namespace bound
