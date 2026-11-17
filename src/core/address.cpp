#include "core/address.h"

#include "util/base58.h"

#include <algorithm>

namespace bound {

std::string encode_address(const PublicKey& pk, const AddressVersion& version) {
    Bytes payload(version.bytes.begin(), version.bytes.begin() + version.size);
    payload.insert(payload.end(), pk.begin(), pk.end());
    return base58check_encode(payload);
}

std::optional<PublicKey> decode_address(std::string_view s, const AddressVersion& version) {
    if (s.size() > 64 || version.size == 0) return std::nullopt;
    const std::optional<Bytes> payload = base58check_decode(s);
    const std::size_t n = version.size;
    if (!payload || payload->size() != n + 32 || !std::equal(version.bytes.begin(), version.bytes.begin() + n, payload->begin())) {
        return std::nullopt;
    }
    PublicKey pk{};  // all 32 bytes are copied in below; {} keeps GCC -O2 from warning that they might not be
    std::copy(payload->begin() + static_cast<std::ptrdiff_t>(n), payload->end(), pk.begin());
    return pk;
}

}  // namespace bound
