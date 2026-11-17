#pragma once

#include "core/amount.h"
#include "core/block.h"
#include "core/chain.h"
#include "core/coins.h"
#include "core/storage.h"
#include "core/transaction.h"
#include "core/uint256.h"
#include "core/validation.h"
#include "crypto/ed25519.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bound {

// Milestone 8: optional indexes over the active chain, for block explorers. The chainstate only
// knows what's unspent; an explorer also asks "where is this transaction?", "what did this address
// ever receive?" and "what spent this output?". Bitcoin Core answers the first with -txindex and
// leaves the others to outside tools; here all three are kept by the node, in a database of their
// own (<datadir>/index), when it runs with --index.
//
// What's stored, by the first byte of the key (integers in keys are big-endian, so they sort by
// value; in values, little-endian):
//   'M'                                  version u32, genesis hash
//   'B'                                  the block the index is in step with
//   't' txid                             block hash, position in the block (u32)
//   's' txid index(u32)                  the txid that spent this output
//   'a' address height(u32) pos(u32)     txid, received (i64), spent (i64): one transaction's
//                                        effect on one address
//   'u' address height(u32) txid idx(u32)  amount (i64), coinbase (u8): an unspent output
//   'y' address                          AddressSummary
// An address is the 32-byte public key. The genesis coinbase, which can never be spent and isn't
// in the UTXO set, is only in 't'.
//
// The index follows the chain as a ChainListener, one atomic write per block, written after the
// chainstate's own. So it can fall a block behind in a crash, and it's simply not updated while
// the node runs without --index: opening it catches up, backing out of blocks that left the
// active chain meanwhile (the chainstate keeps every connected block's undo data for that) and
// reading the ones it missed.
//
// Not thread-safe: use it from the thread that owns the chainstate.

struct TxLocation {
    uint256 block;
    std::uint32_t position = 0;  // its place among the block's transactions

    friend bool operator==(const TxLocation&, const TxLocation&) = default;
};

struct AddressSummary {
    Amount balance = 0;       // unspent now
    Amount received = 0;      // everything ever paid to it, change it paid itself included
    Amount sent = 0;          // everything ever spent from it
    std::uint64_t txs = 0;    // transactions that pay it or spend from it
    std::uint64_t coins = 0;  // unspent outputs

    friend bool operator==(const AddressSummary&, const AddressSummary&) = default;
};

// One transaction's effect on one address.
struct AddressDelta {
    uint256 txid;
    int height = 0;
    std::uint32_t position = 0;
    Amount received = 0;
    Amount spent = 0;

    friend bool operator==(const AddressDelta&, const AddressDelta&) = default;
};

struct AddressCoin {
    OutPoint op;
    Amount amount = 0;
    int height = 0;
    bool coinbase = false;

    friend bool operator==(const AddressCoin&, const AddressCoin&) = default;
};

class ChainIndex final : public ChainListener {
public:
    using Logger = std::function<void(const std::string&)>;

    // Opens the index in `db`, creating it if `db` is empty, and brings it in step with the
    // chain's tip; on a long chain a new index reads every block, which takes a while (`log`
    // hears about the progress). Then follows `chain`, which must outlive it. Throws
    // std::runtime_error if db holds another network's index or is damaged.
    ChainIndex(KeyValueStore& db, Chainstate& chain, Logger log = {});
    ~ChainIndex() override;
    ChainIndex(const ChainIndex&) = delete;
    ChainIndex& operator=(const ChainIndex&) = delete;

    // Where a confirmed transaction is; nullopt if it isn't on the active chain.
    std::optional<TxLocation> find_tx(const uint256& txid) const;
    struct FoundTx {
        Transaction tx;
        const BlockIndex* block = nullptr;
        std::uint32_t position = 0;
    };
    std::optional<FoundTx> read_tx(const uint256& txid) const;  // the same, with the transaction itself
    // The confirmed transaction that spent an output; nullopt if it's unspent or never existed.
    std::optional<uint256> spender(const OutPoint& op) const;

    AddressSummary summary(const PublicKey& address) const;
    // The transactions that touch an address, newest first, leaving out the first `skip`.
    std::vector<AddressDelta> history(const PublicKey& address, std::size_t skip, std::size_t count) const;
    // Its unspent outputs, newest first.
    std::vector<AddressCoin> coins(const PublicKey& address, std::size_t skip, std::size_t count) const;

    const uint256& best_block() const { return best_; }

    void block_connected(const Block& block, const BlockIndex& node) override;
    void block_disconnected(const Block& block, const BlockIndex& node) override;

private:
    // One or more blocks' changes, written together.
    struct Pending {
        WriteBatch batch;
        std::map<PublicKey, AddressSummary> summaries;  // read on first touch, written with the batch
    };

    void create();
    void sync();
    void wipe();
    void apply(const Block& block, const BlockIndex& node, Pending& pending) const;
    void revert(const Block& block, const BlockIndex& node, Pending& pending) const;
    AddressSummary& summary_in(Pending& pending, const PublicKey& address) const;
    BlockUndo undo_of(const BlockIndex& node) const;
    void flush(Pending& pending);

    KeyValueStore& db_;
    Chainstate& chain_;
    Logger log_;
    uint256 best_;
};

}  // namespace bound
