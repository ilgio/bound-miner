#include "core/coins.h"

#include <utility>

std::size_t std::hash<bound::OutPoint>::operator()(const bound::OutPoint& o) const noexcept {
    // txids are hashes already, so mixing in the index is enough.
    return std::hash<bound::uint256>{}(o.txid) ^ (static_cast<std::size_t>(o.index) * 0x9e3779b97f4a7c15ULL);
}

namespace bound {

const Coin* UtxoSet::find(const OutPoint& op) const {
    const auto it = coins_.find(op);
    return it == coins_.end() ? nullptr : &it->second;
}

bool UtxoSet::add(const OutPoint& op, Coin coin) { return coins_.emplace(op, std::move(coin)).second; }

std::optional<Coin> UtxoSet::spend(const OutPoint& op) {
    const auto it = coins_.find(op);
    if (it == coins_.end()) return std::nullopt;
    Coin coin = std::move(it->second);
    coins_.erase(it);
    return coin;
}

Amount UtxoSet::total_value() const {
    Amount total = 0;
    for (const auto& [op, coin] : coins_) total += coin.out.amount;
    return total;
}

}  // namespace bound
