#pragma once

#include "core/amount.h"
#include "core/transaction.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

// Lets OutPoint key unordered containers.
template <>
struct std::hash<bound::OutPoint> {
    std::size_t operator()(const bound::OutPoint& o) const noexcept;
};

namespace bound {

// An unspent output, plus what validation needs to know about where it came from.
struct Coin {
    TxOut out;
    int height = 0;         // height of the block that created it
    bool coinbase = false;  // coinbase outputs must mature before they can be spent

    friend bool operator==(const Coin&, const Coin&) = default;
};

// The UTXO set: every output that exists and hasn't been spent yet. The whole ledger state of
// the coin; balances are just sums over it. Kept whole in memory; from milestone 5 the chainstate
// also keeps a copy on disk, updated with every block it connects or disconnects.
class UtxoSet {
public:
    const Coin* find(const OutPoint& op) const;  // nullptr if there's no such unspent output
    bool contains(const OutPoint& op) const { return find(op) != nullptr; }

    // Adds a coin. Returns false and changes nothing if `op` is already unspent.
    bool add(const OutPoint& op, Coin coin);
    // Removes and returns the coin, or nullopt if there's no such unspent output.
    std::optional<Coin> spend(const OutPoint& op);

    std::size_t size() const { return coins_.size(); }
    Amount total_value() const;  // the sum of every coin: the circulating supply

    // Calls f(const OutPoint&, const Coin&) for every coin, in no particular order.
    template <typename F>
    void for_each(F&& f) const {
        for (const auto& [op, coin] : coins_) f(op, coin);
    }

    friend bool operator==(const UtxoSet&, const UtxoSet&) = default;

private:
    std::unordered_map<OutPoint, Coin> coins_;
};

// What disconnecting a block needs to put back: the coins its transactions spent, in the order
// they were spent (Bitcoin's CBlockUndo).
struct BlockUndo {
    std::vector<Coin> spent;

    friend bool operator==(const BlockUndo&, const BlockUndo&) = default;
};

}  // namespace bound
