#include "util/base58.h"

#include "crypto/sha256.h"

#include <algorithm>
#include <cstdint>

namespace bound {

namespace {

constexpr std::string_view kAlphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

}  // namespace

std::string base58_encode(ByteSpan data) {
    std::size_t zeros = 0;
    while (zeros < data.size() && data[zeros] == 0) ++zeros;
    std::vector<std::uint8_t> digits;  // base 58, least significant first
    for (std::size_t i = zeros; i < data.size(); ++i) {
        int carry = data[i];
        for (std::uint8_t& d : digits) {
            carry += 256 * d;
            d = static_cast<std::uint8_t>(carry % 58);
            carry /= 58;
        }
        while (carry > 0) {
            digits.push_back(static_cast<std::uint8_t>(carry % 58));
            carry /= 58;
        }
    }
    std::string out(zeros, '1');
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) out.push_back(kAlphabet[*it]);
    return out;
}

std::optional<Bytes> base58_decode(std::string_view s) {
    std::size_t ones = 0;
    while (ones < s.size() && s[ones] == '1') ++ones;
    std::vector<std::uint8_t> bytes;  // base 256, least significant first
    for (std::size_t i = ones; i < s.size(); ++i) {
        const std::size_t digit = kAlphabet.find(s[i]);
        if (digit == std::string_view::npos) return std::nullopt;
        int carry = static_cast<int>(digit);
        for (std::uint8_t& b : bytes) {
            carry += 58 * b;
            b = static_cast<std::uint8_t>(carry & 0xff);
            carry >>= 8;
        }
        while (carry > 0) {
            bytes.push_back(static_cast<std::uint8_t>(carry & 0xff));
            carry >>= 8;
        }
    }
    Bytes out(ones, 0);
    out.insert(out.end(), bytes.rbegin(), bytes.rend());
    return out;
}

std::string base58check_encode(ByteSpan payload) {
    Bytes buf(payload.begin(), payload.end());
    const Digest32 check = sha256d(payload);
    buf.insert(buf.end(), check.begin(), check.begin() + 4);
    return base58_encode(buf);
}

std::optional<Bytes> base58check_decode(std::string_view s) {
    std::optional<Bytes> raw = base58_decode(s);
    if (!raw || raw->size() < 4) return std::nullopt;
    const std::size_t n = raw->size() - 4;
    const Digest32 check = sha256d(ByteSpan(*raw).first(n));
    if (!std::equal(check.begin(), check.begin() + 4, raw->begin() + static_cast<std::ptrdiff_t>(n))) return std::nullopt;
    raw->resize(n);
    return raw;
}

}  // namespace bound
