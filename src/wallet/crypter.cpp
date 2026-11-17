#include "wallet/crypter.h"

#include "util/serialize.h"

#include <sodium.h>

#include <array>
#include <stdexcept>

namespace bound {

namespace {

constexpr std::size_t kSaltSize = crypto_pwhash_SALTBYTES;                          // 16
constexpr std::size_t kNonceSize = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;    // 24
constexpr std::size_t kTagSize = crypto_aead_xchacha20poly1305_ietf_ABYTES;         // 16
using Key = std::array<std::uint8_t, crypto_aead_xchacha20poly1305_ietf_KEYBYTES>;  // 32

void ensure_sodium() {
    static const bool ok = sodium_init() >= 0;
    if (!ok) throw std::runtime_error("sodium_init failed");
}

// false if libsodium refuses: settings out of range, or not enough memory.
bool derive_key(Key& key, std::string_view passphrase, const std::uint8_t* salt, const KdfParams& kdf) {
    return crypto_pwhash(key.data(), key.size(), passphrase.data(), passphrase.size(), salt, kdf.ops,
                         static_cast<std::size_t>(kdf.mem), crypto_pwhash_ALG_ARGON2ID13) == 0;
}

}  // namespace

Bytes encrypt_secret(ByteSpan secret, std::string_view passphrase, ByteSpan associated, const KdfParams& kdf) {
    ensure_sodium();
    std::array<std::uint8_t, kSaltSize> salt;
    std::array<std::uint8_t, kNonceSize> nonce;
    randombytes_buf(salt.data(), salt.size());
    randombytes_buf(nonce.data(), nonce.size());
    Key key;
    if (!derive_key(key, passphrase, salt.data(), kdf)) throw std::runtime_error("can't derive the wallet key (out of memory?)");

    Bytes sealed(secret.size() + kTagSize);
    unsigned long long sealed_size = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(sealed.data(), &sealed_size, secret.data(), secret.size(), associated.data(),
                                               associated.size(), nullptr, nonce.data(), key.data());
    sodium_memzero(key.data(), key.size());

    ByteWriter w;
    w.bytes(salt);
    w.u64(kdf.ops);
    w.u64(kdf.mem);
    w.bytes(nonce);
    w.var_bytes(sealed);
    return std::move(w).take();
}

std::optional<Bytes> decrypt_secret(ByteSpan blob, std::string_view passphrase, ByteSpan associated) {
    ensure_sodium();
    ByteReader r(blob);
    const ByteSpan salt = r.bytes(kSaltSize);
    KdfParams kdf;
    kdf.ops = r.u64();
    kdf.mem = r.u64();
    const ByteSpan nonce = r.bytes(kNonceSize);
    const Bytes sealed = r.var_bytes(1 << 16);
    r.expect_end();
    if (sealed.size() < kTagSize) throw DeserializeError("encrypted secret too short");
    if (kdf.ops < crypto_pwhash_OPSLIMIT_MIN || kdf.ops > crypto_pwhash_OPSLIMIT_MAX || kdf.mem < crypto_pwhash_MEMLIMIT_MIN ||
        kdf.mem > (std::uint64_t{1} << 32)) {
        throw DeserializeError("encrypted secret has unusable key derivation settings");
    }

    Key key;
    if (!derive_key(key, passphrase, salt.data(), kdf)) throw std::runtime_error("can't derive the wallet key (out of memory?)");
    Bytes secret(sealed.size() - kTagSize);
    unsigned long long secret_size = 0;
    const int failed = crypto_aead_xchacha20poly1305_ietf_decrypt(secret.data(), &secret_size, nullptr, sealed.data(), sealed.size(),
                                                                  associated.data(), associated.size(), nonce.data(), key.data());
    sodium_memzero(key.data(), key.size());
    if (failed != 0) return std::nullopt;
    return secret;
}

}  // namespace bound
