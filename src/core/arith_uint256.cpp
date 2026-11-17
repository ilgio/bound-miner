#include "core/arith_uint256.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace bound {

arith_uint256::arith_uint256(const uint256& v) {
    const Digest32& b = v.bytes();
    for (std::size_t i = 0; i < limbs_.size(); ++i) {
        limbs_[i] = std::uint32_t{b[4 * i]} | std::uint32_t{b[4 * i + 1]} << 8 | std::uint32_t{b[4 * i + 2]} << 16 |
                    std::uint32_t{b[4 * i + 3]} << 24;
    }
}

uint256 arith_uint256::to_uint256() const {
    Digest32 b;
    for (std::size_t i = 0; i < limbs_.size(); ++i) {
        for (std::size_t j = 0; j < 4; ++j) b[4 * i + j] = static_cast<std::uint8_t>(limbs_[i] >> (8 * j));
    }
    return uint256(b);
}

int arith_uint256::bits() const {
    for (int i = 7; i >= 0; --i) {
        if (limbs_[i] != 0) return 32 * i + static_cast<int>(std::bit_width(limbs_[i]));
    }
    return 0;
}

double arith_uint256::to_double() const {
    double result = 0;
    for (int i = 7; i >= 0; --i) result = result * 4294967296.0 + limbs_[i];
    return result;
}

arith_uint256& arith_uint256::operator+=(const arith_uint256& b) {
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < limbs_.size(); ++i) {
        carry += std::uint64_t{limbs_[i]} + b.limbs_[i];
        limbs_[i] = static_cast<std::uint32_t>(carry);
        carry >>= 32;
    }
    return *this;
}

arith_uint256& arith_uint256::operator-=(const arith_uint256& b) { return *this += ~b + 1; }

// Schoolbook binary long division, as in Bitcoin: line the divisor up under the dividend's top
// bit, then shift it back down one bit at a time, subtracting wherever it fits.
arith_uint256& arith_uint256::operator/=(const arith_uint256& b) {
    const int div_bits = b.bits();
    if (div_bits == 0) throw std::domain_error("division by zero");
    arith_uint256 num = *this;
    *this = 0;
    const int num_bits = num.bits();
    if (div_bits > num_bits) return *this;
    int shift = num_bits - div_bits;
    arith_uint256 div = b << static_cast<unsigned>(shift);
    for (; shift >= 0; --shift, div >>= 1) {
        if (num >= div) {
            num -= div;
            limbs_[shift / 32] |= 1u << (shift % 32);
        }
    }
    return *this;
}

arith_uint256& arith_uint256::operator<<=(unsigned shift) {
    const arith_uint256 a = *this;
    limbs_.fill(0);
    const unsigned k = shift / 32;
    const unsigned s = shift % 32;
    for (unsigned i = 0; i + k < 8; ++i) {
        limbs_[i + k] |= a.limbs_[i] << s;
        if (s != 0 && i + k + 1 < 8) limbs_[i + k + 1] |= a.limbs_[i] >> (32 - s);
    }
    return *this;
}

arith_uint256& arith_uint256::operator>>=(unsigned shift) {
    const arith_uint256 a = *this;
    limbs_.fill(0);
    const unsigned k = shift / 32;
    const unsigned s = shift % 32;
    for (unsigned i = k; i < 8; ++i) {
        limbs_[i - k] |= a.limbs_[i] >> s;
        if (s != 0 && i > k) limbs_[i - k - 1] |= a.limbs_[i] << (32 - s);
    }
    return *this;
}

arith_uint256 arith_uint256::operator~() const {
    arith_uint256 r;
    for (std::size_t i = 0; i < limbs_.size(); ++i) r.limbs_[i] = ~limbs_[i];
    return r;
}

std::strong_ordering operator<=>(const arith_uint256& a, const arith_uint256& b) {
    for (int i = 7; i >= 0; --i) {
        if (const auto c = a.limbs_[i] <=> b.limbs_[i]; c != 0) return c;
    }
    return std::strong_ordering::equal;
}

std::optional<arith_uint256> mul_div(const arith_uint256& a, std::uint32_t num, std::uint32_t den) {
    if (den == 0) throw std::domain_error("division by zero");
    // a * num in nine 32-bit limbs; each step's product plus carry stays below 2^64.
    std::array<std::uint32_t, 9> wide{};
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        carry += std::uint64_t{a.limbs_[i]} * num;
        wide[i] = static_cast<std::uint32_t>(carry);
        carry >>= 32;
    }
    wide[8] = static_cast<std::uint32_t>(carry);

    // Divide by den from the top limb down; the remainder is always below den, so fits 32 bits.
    std::uint64_t rem = 0;
    for (std::size_t i = wide.size(); i-- > 0;) {
        const std::uint64_t cur = rem << 32 | wide[i];
        wide[i] = static_cast<std::uint32_t>(cur / den);
        rem = cur % den;
    }
    if (wide[8] != 0) return std::nullopt;
    arith_uint256 result;
    std::copy_n(wide.begin(), 8, result.limbs_.begin());
    return result;
}

}  // namespace bound
