#pragma once

#include "core/amount.h"
#include "core/coins.h"
#include "core/params.h"
#include "core/transaction.h"
#include "crypto/ed25519.h"

#include <optional>
#include <utility>
#include <vector>

namespace bound {

// A minimal single-key wallet over the UTXO set: enough to pay someone. The node's real wallet
// (seed-derived keys, encryption, history, fee bumps) is milestone 7's src/wallet; these helpers
// stay for `boundd demo`, offline `balance` and tests.

// Sum of every coin paying `owner`, mature or not.
Amount balance(const UtxoSet& utxos, const PublicKey& owner);

// owner's coins that a block at `spend_height` may spend (immature coinbase outputs excluded),
// oldest first, ties broken by outpoint so the order is deterministic.
std::vector<std::pair<OutPoint, Coin>> spendable_coins(const UtxoSet& utxos, const PublicKey& owner,
                                                       int spend_height, const ConsensusParams& params);

// A signed transaction paying `amount` to `to` plus `fee` to the miner, funded by from's oldest
// spendable coins, with any change going back to `from` as a second output. nullopt if
// amount <= 0, fee < 0, or from can't afford amount + fee at `spend_height`.
std::optional<Transaction> create_payment(const UtxoSet& utxos, const KeyPair& from, const PublicKey& to,
                                          Amount amount, Amount fee, int spend_height,
                                          const ConsensusParams& params);

}  // namespace bound
