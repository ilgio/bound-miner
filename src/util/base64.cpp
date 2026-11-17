#include "util/base64.h"

#include <algorithm>
#include <cstdint>

namespace bound {

namespace {

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int value_of(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64_encode(ByteSpan data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::size_t n = std::min<std::size_t>(3, data.size() - i);
        std::uint32_t group = std::uint32_t{data[i]} << 16;
        if (n > 1) group |= std::uint32_t{data[i + 1]} << 8;
        if (n > 2) group |= data[i + 2];
        out += kAlphabet[group >> 18 & 63];
        out += kAlphabet[group >> 12 & 63];
        out += n > 1 ? kAlphabet[group >> 6 & 63] : '=';
        out += n > 2 ? kAlphabet[group & 63] : '=';
    }
    return out;
}

std::optional<Bytes> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    Bytes out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        std::size_t padding = 0;
        if (last && text[i + 3] == '=') padding = text[i + 2] == '=' ? 2 : 1;
        std::uint32_t group = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            int v = 0;
            if (k < 4 - padding) {
                v = value_of(text[i + k]);
                if (v < 0) return std::nullopt;
            }
            group = group << 6 | static_cast<std::uint32_t>(v);
        }
        // The bits padding stands for must be zero, or two strings would decode the same.
        if ((padding == 1 && (group & 0xff) != 0) || (padding == 2 && (group & 0xffff) != 0)) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(group >> 16));
        if (padding < 2) out.push_back(static_cast<std::uint8_t>(group >> 8));
        if (padding < 1) out.push_back(static_cast<std::uint8_t>(group));
    }
    return out;
}

}  // namespace bound
