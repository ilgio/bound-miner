#include "core/amount.h"

#include <format>

namespace bound {

std::string format_coins(Amount a) {
    // Negate as unsigned: -INT64_MIN would overflow.
    const std::uint64_t abs = a < 0 ? 0 - static_cast<std::uint64_t>(a) : static_cast<std::uint64_t>(a);
    return std::format("{}{}.{:08}", a < 0 ? "-" : "", abs / kCoin, abs % kCoin);
}

std::optional<Amount> parse_coins(std::string_view text) {
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    const std::string_view fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || whole.size() > 8 || fraction.size() > 8) return std::nullopt;  // 8 digits: up to 99,999,999
    if (dot != std::string_view::npos && fraction.empty()) return std::nullopt;         // "1." is a typo

    Amount units = 0;
    for (const char c : whole) {
        if (c < '0' || c > '9') return std::nullopt;
        units = units * 10 + (c - '0');
    }
    units *= kCoin;
    Amount scale = kCoin;
    for (const char c : fraction) {
        if (c < '0' || c > '9') return std::nullopt;
        scale /= 10;
        units += (c - '0') * scale;
    }
    if (units > kMaxMoney) return std::nullopt;
    return negative ? -units : units;
}

}  // namespace bound
