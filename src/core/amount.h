#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bound {

// Money is an integer count of base units. Never floating point.
using Amount = std::int64_t;

inline constexpr Amount kCoin = 100'000'000;             // 1 BOUND = 10^8 base units
inline constexpr Amount kMaxMoney = 21'000'000 * kCoin;  // range-check bound, like Bitcoin's MAX_MONEY;
                                                         // the real supply is 20,999,999.727 BOUND

inline constexpr bool money_range(Amount a) { return a >= 0 && a <= kMaxMoney; }

// Milestone 7: amounts as text, the way RPC shows and takes them. Always 8 decimals:
// 150000000 -> "1.50000000", -191 -> "-0.00000191".
std::string format_coins(Amount a);
// Inverse, exactly (no floating point): "1.5", "0.00000191", "7", "-2.25". At most 8 decimals;
// no exponent, no spaces, no "+". nullopt if malformed or beyond ±kMaxMoney.
std::optional<Amount> parse_coins(std::string_view text);

}  // namespace bound
