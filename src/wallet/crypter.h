#pragma once

#include "util/bytes.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace bound {

// Milestone 7: the wallet's secret under a passphrase (Bitcoin's crypter.cpp, with modern parts),
// all through libsodium:
//   key  = Argon2id(passphrase, salt, ops, mem), 32 bytes: slow and memory-hard, so guessing
//          passphrases is expensive
//   blob = salt (16) | ops (u64) | mem (u64) | nonce (24) | XChaCha20-Poly1305(secret, ad)
// The Poly1305 tag authenticates the ciphertext and the associated data: a wrong passphrase, a
// changed blob or different associated data all fail the same way.

struct KdfParams {
    std::uint64_t ops = 2;          // crypto_pwhash_OPSLIMIT_INTERACTIVE
    std::uint64_t mem = 64u << 20;  // crypto_pwhash_MEMLIMIT_INTERACTIVE: 64 MiB
};
// The cheapest settings libsodium allows: for tests only.
inline constexpr KdfParams kTestKdf{1, 8192};

// Throws std::runtime_error if libsodium refuses (out of memory for `mem`).
Bytes encrypt_secret(ByteSpan secret, std::string_view passphrase, ByteSpan associated, const KdfParams& kdf);
// nullopt if the passphrase is wrong, or the blob or associated data aren't the ones encrypted.
// Throws DeserializeError if the blob is malformed, or its KDF settings are out of libsodium's range.
std::optional<Bytes> decrypt_secret(ByteSpan blob, std::string_view passphrase, ByteSpan associated);

}  // namespace bound
