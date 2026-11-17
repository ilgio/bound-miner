#include "crypto/ed25519.h"

#include <stdexcept>

namespace bound {

namespace {
void ensure_sodium() {
    static const bool ok = sodium_init() >= 0;
    if (!ok) throw std::runtime_error("sodium_init failed");
}
}  // namespace

KeyPair KeyPair::generate() {
    ensure_sodium();
    KeyPair k;
    crypto_sign_keypair(k.pk_.data(), k.sk_.data());
    return k;
}

KeyPair KeyPair::from_seed(const Seed& seed) {
    ensure_sodium();
    KeyPair k;
    crypto_sign_seed_keypair(k.pk_.data(), k.sk_.data(), seed.data());
    return k;
}

KeyPair::KeyPair(KeyPair&& other) noexcept : sk_(other.sk_), pk_(other.pk_) {
    sodium_memzero(other.sk_.data(), other.sk_.size());
}

KeyPair& KeyPair::operator=(KeyPair&& other) noexcept {
    if (this != &other) {
        sk_ = other.sk_;
        pk_ = other.pk_;
        sodium_memzero(other.sk_.data(), other.sk_.size());
    }
    return *this;
}

KeyPair::~KeyPair() { sodium_memzero(sk_.data(), sk_.size()); }

const PublicKey& KeyPair::public_key() const { return pk_; }

Signature KeyPair::sign(ByteSpan message) const {
    Signature sig;
    crypto_sign_detached(sig.data(), nullptr, message.data(), message.size(), sk_.data());
    return sig;
}

bool verify(const PublicKey& pk, ByteSpan message, const Signature& sig) {
    ensure_sodium();
    return crypto_sign_verify_detached(sig.data(), message.data(), message.size(), pk.data()) == 0;
}

}  // namespace bound
