#pragma once

#include "util/bytes.h"

#include <optional>
#include <string>
#include <string_view>

namespace bound {

// Lowercase hex of the bytes in the order given: {0x01, 0xab} -> "01ab".
std::string to_hex(ByteSpan bytes);

// Inverse of to_hex. Accepts either case. Returns nullopt on odd length or any character that
// isn't a hex digit (no "0x" prefix, no whitespace). "" decodes to an empty vector.
std::optional<Bytes> from_hex(std::string_view hex);

}  // namespace bound
