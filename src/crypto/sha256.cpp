#include "crypto/sha256.h"

#include <stdexcept>

namespace bound {

namespace {
void ensure_sodium() {
    static const bool ok = sodium_init() >= 0;
    if (!ok) throw std::runtime_error("sodium_init failed");
}
}  // namespace

Sha256::Sha256() { reset(); }

void Sha256::reset() {
    ensure_sodium();
    crypto_hash_sha256_init(&state_);
}

Sha256& Sha256::write(ByteSpan data) {
    crypto_hash_sha256_update(&state_, data.data(), data.size());
    return *this;
}

Digest32 Sha256::finalize() {
    Digest32 out;
    crypto_hash_sha256_final(&state_, out.data());
    return out;
}

Digest32 sha256(ByteSpan data) { return Sha256().write(data).finalize(); }

Digest32 sha256d(ByteSpan data) { return sha256(sha256(data)); }

Digest32 tagged_hash(std::string_view tag, ByteSpan data) {
    const Digest32 t = sha256(bytes_of(tag));
    return Sha256().write(t).write(t).write(data).finalize();
}

}  // namespace bound
