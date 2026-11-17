#pragma once

#include "core/params.h"
#include "crypto/ed25519.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bound {

// Base58Check(version || 32-byte Ed25519 public key). Longer than a Bitcoin address because
// BOUND pays to the key itself, not to a 20-byte hash of it. The version (the network's
// ConsensusParams::address_version) fixes the first character and keeps networks apart:
//   - 1 byte (testnet, regtest): 37 bytes, 51 characters; only the digits 2-9 can be fixed so;
//   - 2 bytes (mainnet, milestone 10): 38 bytes, 52 characters, starting with 'B'.
std::string encode_address(const PublicKey& pk, const AddressVersion& version);

// The key, or nullopt unless s is valid Base58Check carrying exactly the version and a 32-byte key.
// Rejects strings over 64 characters before decoding: Base58 decoding is quadratic, and
// addresses come from users and peers.
std::optional<PublicKey> decode_address(std::string_view s, const AddressVersion& version);

}  // namespace bound
