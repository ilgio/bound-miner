#include "core/wallet.h"

#include <algorithm>
#include <tuple>

namespace bound {

Amount balance(const UtxoSet& utxos, const PublicKey& owner) {
    Amount total = 0;
    utxos.for_each([&](const OutPoint&, const Coin& coin) {
        if (coin.out.recipient == owner) total += coin.out.amount;
    });
    return total;
}

std::vector<std::pair<OutPoint, Coin>> spendable_coins(const UtxoSet& utxos, const PublicKey& owner,
                                                       int spend_height, const ConsensusParams& params) {
    std::vector<std::pair<OutPoint, Coin>> coins;
    utxos.for_each([&](const OutPoint& op, const Coin& coin) {
        if (coin.out.recipient != owner) return;
        if (coin.coinbase && spend_height - coin.height < params.coinbase_maturity) return;
        coins.emplace_back(op, coin);
    });
    std::sort(coins.begin(), coins.end(), [](const auto& a, const auto& b) {
        return std::tie(a.second.height, a.first) < std::tie(b.second.height, b.first);
    });
    return coins;
}

std::optional<Transaction> create_payment(const UtxoSet& utxos, const KeyPair& from, const PublicKey& to,
                                          Amount amount, Amount fee, int spend_height,
                                          const ConsensusParams& params) {
    if (amount <= 0 || fee < 0 || !money_range(amount) || !money_range(fee) || !money_range(amount + fee)) {
        return std::nullopt;
    }
    const Amount needed = amount + fee;
    Transaction tx;
    Amount gathered = 0;
    for (const auto& [op, coin] : spendable_coins(utxos, from.public_key(), spend_height, params)) {
        if (gathered >= needed) break;
        tx.inputs.push_back(op);
        gathered += coin.out.amount;
    }
    if (gathered < needed) return std::nullopt;

    tx.outputs.push_back(TxOut{amount, to});
    if (gathered > needed) tx.outputs.push_back(TxOut{gathered - needed, from.public_key()});
    sign_all_inputs(tx, from);
    return tx;
}

}  // namespace bound
