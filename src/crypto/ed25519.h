#pragma once

#include "util/bytes.h"

#include <sodium.h>

#include <array>
#include <cstdint>

namespace bound {

using PublicKey = std::array<std::uint8_t, 32>;
using Signature = std::array<std::uint8_t, 64>;
using Seed = std::array<std::uint8_t, 32>;  // what RFC 8032 calls the private key

// Ed25519 (RFC 8032) through libsodium's crypto_sign_*; never hand-roll this. Whatever
// crypto_sign_verify_detached accepts *is* BOUND's signature consensus rule (it rejects
// non-canonical S and small-order keys; Ed25519 libraries disagree on such edge cases, so
// switching libraries later would be a consensus change).
class KeyPair {
public:
    static KeyPair generate();                   // fresh key from libsodium's CSPRNG
    static KeyPair from_seed(const Seed& seed);  // deterministic

    // Secrets don't get copied around: move-only, and the destructor wipes the secret with
    // sodium_memzero. A moved-from KeyPair may only be destroyed or assigned to.
    KeyPair(const KeyPair&) = delete;
    KeyPair& operator=(const KeyPair&) = delete;
    KeyPair(KeyPair&& other) noexcept;
    KeyPair& operator=(KeyPair&& other) noexcept;
    ~KeyPair();

    const PublicKey& public_key() const;
    Signature sign(ByteSpan message) const;

private:
    KeyPair() = default;
    std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES> sk_{};  // libsodium layout: seed || public key
    PublicKey pk_{};
};

// True iff sig is a valid signature of message under pk.
bool verify(const PublicKey& pk, ByteSpan message, const Signature& sig);

}  // namespace bound
