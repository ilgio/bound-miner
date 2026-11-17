#pragma once

#include "util/bytes.h"

#include <sodium.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace bound {

using Digest32 = std::array<std::uint8_t, 32>;

// Incremental SHA-256 (FIPS 180-4), wrapping libsodium's crypto_hash_sha256_*. Copyable, and a
// copy carries the full mid-hash state.
class Sha256 {
public:
    Sha256();
    Sha256& write(ByteSpan data);
    Digest32 finalize();  // afterwards only reset() is valid
    void reset();

private:
    crypto_hash_sha256_state state_;
};

Digest32 sha256(ByteSpan data);

// sha256(sha256(data)): Bitcoin's hash for headers and txids.
Digest32 sha256d(ByteSpan data);

// BIP340-style tagged hash: sha256(sha256(tag) || sha256(tag) || data). BOUND gives every new
// kind of hash it defines its own tag, so hashes of different kinds of data can never collide.
Digest32 tagged_hash(std::string_view tag, ByteSpan data);

}  // namespace bound
