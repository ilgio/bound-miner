#include "core/uint256.h"

#include "util/hex.h"

#include <algorithm>
#include <cstring>

namespace bound {

std::string uint256::to_hex() const {
    Digest32 reversed;
    std::reverse_copy(bytes_.begin(), bytes_.end(), reversed.begin());
    return bound::to_hex(reversed);
}

std::optional<uint256> uint256::from_hex(std::string_view display_hex) {
    if (display_hex.size() != 64) return std::nullopt;
    const std::optional<Bytes> b = bound::from_hex(display_hex);
    if (!b) return std::nullopt;
    Digest32 d;
    std::reverse_copy(b->begin(), b->end(), d.begin());
    return uint256(d);
}

bool uint256::is_zero() const {
    return std::all_of(bytes_.begin(), bytes_.end(), [](std::uint8_t b) { return b == 0; });
}

std::strong_ordering operator<=>(const uint256& a, const uint256& b) {
    for (int i = 31; i >= 0; --i) {
        if (const auto c = a.bytes_[i] <=> b.bytes_[i]; c != 0) return c;
    }
    return std::strong_ordering::equal;
}

}  // namespace bound

std::size_t std::hash<bound::uint256>::operator()(const bound::uint256& v) const noexcept {
    std::uint64_t x;
    std::memcpy(&x, v.bytes().data(), sizeof x);
    return static_cast<std::size_t>(x);
}
