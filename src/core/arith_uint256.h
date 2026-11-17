#pragma once

#include "core/uint256.h"

#include <array>
#include <compare>
#include <cstdint>
#include <optional>

namespace bound {

// Milestone 4: a 256-bit unsigned integer with arithmetic, for targets and chain work (Bitcoin's
// arith_uint256). uint256 stays a plain 32-byte value; convert explicitly between the two.
// Like any unsigned type, + and - wrap around modulo 2^256.
class arith_uint256 {
public:
    constexpr arith_uint256() = default;  // zero
    // Implicit, like an integer literal, so `work + 1` and `x == 0` read naturally.
    constexpr arith_uint256(std::uint64_t v)
        : limbs_{static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(v >> 32)} {}
    explicit arith_uint256(const uint256& v);
    uint256 to_uint256() const;

    // Number of significant bits: 0 for zero, 256 when the top bit is set.
    int bits() const;
    // The nearest double. For display (difficulty, hash counts) only, never for consensus.
    double to_double() const;

    arith_uint256& operator+=(const arith_uint256& b);
    arith_uint256& operator-=(const arith_uint256& b);
    // Rounds down. Throws std::domain_error when b is zero.
    arith_uint256& operator/=(const arith_uint256& b);
    arith_uint256& operator<<=(unsigned shift);  // shifting by 256 or more gives zero
    arith_uint256& operator>>=(unsigned shift);
    arith_uint256 operator~() const;

    friend arith_uint256 operator+(arith_uint256 a, const arith_uint256& b) { return a += b; }
    friend arith_uint256 operator-(arith_uint256 a, const arith_uint256& b) { return a -= b; }
    friend arith_uint256 operator/(arith_uint256 a, const arith_uint256& b) { return a /= b; }
    friend arith_uint256 operator<<(arith_uint256 a, unsigned shift) { return a <<= shift; }
    friend arith_uint256 operator>>(arith_uint256 a, unsigned shift) { return a >>= shift; }

    friend std::strong_ordering operator<=>(const arith_uint256& a, const arith_uint256& b);
    friend bool operator==(const arith_uint256& a, const arith_uint256& b) = default;

    friend std::optional<arith_uint256> mul_div(const arith_uint256& a, std::uint32_t num, std::uint32_t den);

private:
    std::array<std::uint32_t, 8> limbs_{};  // limbs_[0] is the least significant
};

// floor(a * num / den), exact even when a * num needs more than 256 bits: the difficulty
// retarget multiplies targets close to 2^255 by timespans. nullopt if the result itself doesn't
// fit in 256 bits. Throws std::domain_error when den is zero.
std::optional<arith_uint256> mul_div(const arith_uint256& a, std::uint32_t num, std::uint32_t den);

}  // namespace bound
