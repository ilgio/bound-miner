#pragma once

#include "util/bytes.h"

#include <optional>
#include <string>
#include <string_view>

namespace bound {

// Bitcoin's Base58: the bytes as one big-endian number written in base 58 with the alphabet
// 123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz (no 0, O, I or l), plus one
// leading '1' per leading zero byte.
std::string base58_encode(ByteSpan data);
// nullopt on any character outside the alphabet, whitespace included. "" decodes to empty.
std::optional<Bytes> base58_decode(std::string_view s);

// Base58Check: base58(payload || first 4 bytes of sha256d(payload)).
std::string base58check_encode(ByteSpan payload);
// The payload, or nullopt if s isn't Base58, decodes to fewer than 4 bytes, or the checksum
// doesn't match.
std::optional<Bytes> base58check_decode(std::string_view s);

}  // namespace bound
