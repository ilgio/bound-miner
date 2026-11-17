#pragma once

#include "util/bytes.h"

#include <optional>
#include <string>
#include <string_view>

namespace bound {

// Milestone 7: standard Base64 with '=' padding (RFC 4648 section 4), for HTTP Basic auth.
std::string base64_encode(ByteSpan data);
// nullopt unless `text` is canonical padded Base64: length a multiple of 4, only the alphabet, '='
// only at the end, and no stray bits in the last character.
std::optional<Bytes> base64_decode(std::string_view text);

}  // namespace bound
