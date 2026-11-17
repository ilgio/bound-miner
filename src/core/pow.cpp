#include "core/pow.h"

#include "core/block.h"
#include "core/boundhash.h"
#include "core/chain.h"
#include "core/params.h"

#include <algorithm>
#include <stdexcept>

namespace bound {

std::optional<uint256> target_from_compact(std::uint32_t bits) {
    const int size = static_cast<int>(bits >> 24);
    std::uint32_t word = bits & 0x007fffff;
    if (size <= 3) word >>= 8 * (3 - size);
    const bool negative = word != 0 && (bits & 0x00800000) != 0;
    const bool overflow = word != 0 && (size > 34 || (word > 0xff && size > 33) || (word > 0xffff && size > 32));
    if (negative || overflow) return std::nullopt;

    Digest32 b{};
    const int low = size <= 3 ? 0 : size - 3;  // byte index of the mantissa's low byte
    for (int i = 0; i < 3; ++i) {
        const int pos = low + i;
        if (pos < 32) b[pos] = static_cast<std::uint8_t>(word >> (8 * i));
    }
    return uint256(b);
}

std::uint32_t compact_from_target(const uint256& target) {
    const Digest32& b = target.bytes();
    int size = 32;
    while (size > 0 && b[size - 1] == 0) --size;
    std::uint32_t compact = 0;
    for (int i = 0; i < 3; ++i) {
        const int pos = size - 1 - i;
        compact <<= 8;
        if (pos >= 0) compact |= b[pos];
    }
    if (compact & 0x00800000) {
        compact >>= 8;
        ++size;
    }
    return compact | (static_cast<std::uint32_t>(size) << 24);
}

bool check_proof_of_work(const uint256& hash, std::uint32_t bits, const uint256& pow_limit) {
    const std::optional<uint256> target = target_from_compact(bits);
    if (!target || target->is_zero() || *target > pow_limit) return false;
    return hash <= *target;
}

uint256 pow_hash(const BlockHeader& header, const ConsensusParams& params) { return bound_hash(header, params.pow_lines); }

bool check_proof_of_work(const BlockHeader& header, const ConsensusParams& params) {
    const std::optional<uint256> target = target_from_compact(header.bits);
    if (!target || target->is_zero() || *target > params.pow_limit) return false;
    return pow_hash(header, params) <= *target;
}

arith_uint256 block_proof(std::uint32_t bits) {
    const std::optional<uint256> decoded = target_from_compact(bits);
    if (!decoded || decoded->is_zero()) return 0;
    // 2^256 doesn't fit in 256 bits, but 2^256 / (t + 1) == (2^256 - t - 1) / (t + 1) + 1, and
    // 2^256 - t - 1 is ~t. (t + 1 can't wrap: no compact value decodes to 2^256 - 1.)
    const arith_uint256 target(*decoded);
    return ~target / (target + 1) + 1;
}

std::uint32_t retarget(std::uint32_t bits, std::int64_t actual_timespan, std::int64_t expected_timespan,
                       const uint256& pow_limit) {
    if (expected_timespan <= 0 || expected_timespan > (std::int64_t{1} << 30)) {
        throw std::invalid_argument("expected_timespan out of range");
    }
    const std::int64_t actual = std::clamp(actual_timespan, expected_timespan / 4, expected_timespan * 4);
    // target * actual can need more than 256 bits when the target is near a high pow_limit, so
    // multiply and divide in one exact step.
    const std::optional<arith_uint256> target =
        mul_div(arith_uint256(target_from_compact(bits).value()), static_cast<std::uint32_t>(actual),
                static_cast<std::uint32_t>(expected_timespan));
    if (!target || *target > arith_uint256(pow_limit)) return compact_from_target(pow_limit);
    return compact_from_target(target->to_uint256());
}

// Milestone 11. The specification's CalculateASERT, line for line, except where it multiplies the
// anchor target by the factor in 256 bits: here that product is formed and scaled in one exact
// mul_div step, so it can't overflow whatever the pow_limit. Where the specification applies, the
// two agree exactly: floor(x / 2^a / 2^b) == floor(x / 2^(a + b)), and mul_div is exact.
arith_uint256 asert_target(const arith_uint256& anchor_target, std::int64_t time_diff, std::int64_t height_diff,
                           std::int64_t spacing, std::int64_t half_life, const arith_uint256& pow_limit) {
    if (anchor_target == 0 || anchor_target > pow_limit) throw std::invalid_argument("asert: anchor target out of range");
    if (height_diff < 0 || spacing <= 0 || half_life <= 0) throw std::invalid_argument("asert: bad parameters");
    constexpr std::int64_t kLagLimit = std::int64_t{1} << 47;
    if (height_diff > kLagLimit / spacing) throw std::invalid_argument("asert: height difference too large");
    const std::int64_t lag = time_diff - spacing * height_diff;  // seconds behind the schedule
    if (lag <= -kLagLimit || lag >= kLagLimit) throw std::invalid_argument("asert: too far off schedule");

    // The exponent in 1/65536ths: (time_diff - spacing * (height_diff + 1)) / half_life, the
    // division truncating toward zero as the specification's C++ does.
    const std::int64_t exponent = ((lag - spacing) * 65536) / half_life;
    // Its integer part, rounded toward minus infinity (an arithmetic shift, standard since C++20),
    // and its fractional part in [0, 1).
    std::int64_t shifts = exponent >> 16;
    const auto frac = static_cast<std::uint16_t>(exponent);
    // 65536 * 2^(frac / 65536), by the specification's cubic (error under 0.013%).
    const std::uint64_t f = frac;
    const auto factor = static_cast<std::uint32_t>(
        65536 + ((195766423245049ULL * f + 971821376ULL * f * f + 5127ULL * f * f * f + (1ULL << 47)) >> 48));

    // anchor_target * factor * 2^(shifts - 16), rounded down. nullopt: beyond 256 bits, so far
    // above pow_limit that the clamp below applies.
    shifts -= 16;
    std::optional<arith_uint256> next;
    if (shifts >= 0) {
        next = mul_div(anchor_target, factor, 1);
        if (next) {
            const auto up = static_cast<unsigned>(std::min<std::int64_t>(shifts, 256));
            const arith_uint256 shifted = *next << up;
            if ((shifted >> up) != *next) {
                next.reset();  // the shift lost high bits
            } else {
                next = shifted;
            }
        }
    } else {
        const std::int64_t down = -shifts;
        if (down <= 31) {
            next = mul_div(anchor_target, factor, std::uint32_t{1} << down);
        } else {
            next = mul_div(anchor_target, factor, std::uint32_t{1} << 31);
            if (next) *next >>= static_cast<unsigned>(std::min<std::int64_t>(down - 31, 256));
        }
    }

    arith_uint256 target = next ? *next : pow_limit;
    if (target == 0) {
        target = 1;  // 0 isn't a valid target, but 1 is
    } else if (target > pow_limit) {
        target = pow_limit;
    }
    return target;
}

std::uint32_t next_work_required(const BlockIndex& parent, const ConsensusParams& params) {
    return next_work_required(
        parent.height, parent.header.bits, parent.header.time,
        [&](int h) { return static_cast<std::int64_t>(parent.ancestor(h)->header.time); }, params);
}

std::uint32_t next_work_required(int parent_height, std::uint32_t parent_bits, std::int64_t parent_time,
                                 const std::function<std::int64_t(int)>& time_at, const ConsensusParams& params) {
    if (params.pow_no_retargeting) return parent_bits;  // regtest
    if (params.asert_half_life > 0) {
        // Milestone 11 (mainnet). The anchor is the genesis block: it has no parent, so its own
        // time stands in for its parent's, as the specification has it for a genesis anchor.
        const arith_uint256 anchor(target_from_compact(params.genesis.bits).value());
        const arith_uint256 target =
            asert_target(anchor, parent_time - std::int64_t{params.genesis.time}, parent_height, params.target_spacing,
                         params.asert_half_life, arith_uint256(params.pow_limit));
        return compact_from_target(target.to_uint256());
    }
    const int interval = params.retarget_interval;
    if ((parent_height + 1) % interval != 0) return parent_bits;

    // The window starts at the last block of the previous period, so consecutive windows share an
    // endpoint and together measure every second of the chain. Bitcoin starts one block later
    // (2015 gaps for 2016 blocks), which leaves the gap between periods unmeasured: the off-by-one
    // behind the timewarp attack. Bitcoin can only patch that with a soft fork (BIP54); a new
    // chain can fix the window itself, as Litecoin did.
    const int first = std::max(0, parent_height - interval);
    const std::int64_t actual = parent_time - time_at(first);
    const std::int64_t expected = std::int64_t{parent_height - first} * params.target_spacing;
    return retarget(parent_bits, actual, expected, params.pow_limit);
}

double difficulty(std::uint32_t bits, const ConsensusParams& params) {
    const std::optional<uint256> target = target_from_compact(bits);
    if (!target || target->is_zero()) return 0;
    return arith_uint256(params.pow_limit).to_double() / arith_uint256(*target).to_double();
}

}  // namespace bound
