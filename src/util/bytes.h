#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bound {

using Bytes = std::vector<std::uint8_t>;
using ByteSpan = std::span<const std::uint8_t>;

// Views a string's characters as bytes (no copy, no terminator).
inline ByteSpan bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

}  // namespace bound
