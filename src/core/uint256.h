#pragma once

#include "crypto/sha256.h"

#include <compare>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace bound {

// 32 bytes read as a little-endian unsigned integer (bytes()[0] least significant), exactly like
// Bitcoin's uint256. Holds block hashes, txids, merkle roots and PoW targets.
class uint256 {
public:
    constexpr uint256() = default;  // zero
    explicit constexpr uint256(const Digest32& bytes) : bytes_(bytes) {}

    // Display hex: most significant byte first, i.e. bytes()[31] first. That's the reverse of
    // the in-memory order, and how block explorers print hashes (leading zeros first). 64 chars.
    std::string to_hex() const;
    // Inverse of to_hex(). nullopt unless exactly 64 hex digits (either case).
    static std::optional<uint256> from_hex(std::string_view display_hex);

    const Digest32& bytes() const { return bytes_; }
    bool is_zero() const;

    // Numeric order, most significant byte (bytes()[31]) first. A defaulted <=> would compare
    // bytes()[0] first, which is wrong for this layout.
    friend std::strong_ordering operator<=>(const uint256& a, const uint256& b);
    friend bool operator==(const uint256& a, const uint256& b) = default;

private:
    Digest32 bytes_{};
};

}  // namespace bound

// Lets uint256 key std::unordered_map / std::unordered_set.
template <>
struct std::hash<bound::uint256> {
    std::size_t operator()(const bound::uint256& v) const noexcept;
};
