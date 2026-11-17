#pragma once

#include "core/amount.h"
#include "core/block.h"
#include "core/coins.h"
#include "core/transaction.h"
#include "core/uint256.h"
#include "core/validation.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bound {

// Milestone 7: the mempool (Bitcoin's CTxMemPool plus the policy half of validation.cpp). Valid
// transactions waiting for a block, checked against the active chain's UTXO set and against each
// other. A transaction may spend outputs of others still waiting here (an unconfirmed chain).

// A fee rate in base units per 1,000 bytes (Bitcoin's CFeeRate), so rates like 1.5 units per byte
// stay integers.
struct FeeRate {
    Amount per_kb = 0;

    // The rate `fee` pays for `bytes`, rounded down.
    static FeeRate of(Amount fee, std::size_t bytes);
    // What `bytes` must pay at this rate, rounded up: a fee of fee(n) for n bytes meets the rate.
    Amount fee(std::size_t bytes) const;

    friend auto operator<=>(const FeeRate&, const FeeRate&) = default;
};

// fee_a / size_a against fee_b / size_b, exactly (no floating point, no overflow). Fees >= 0.
std::strong_ordering compare_rates(Amount fee_a, std::size_t size_a, Amount fee_b, std::size_t size_b);

// The serialized size of a transaction with these counts and a memo of memo_size bytes.
std::size_t transaction_size(std::size_t inputs, std::size_t outputs, std::size_t memo_size);

// Relay policy: which valid transactions this node keeps and passes on. Not consensus: a block may
// contain transactions that break these.
struct MempoolOptions {
    FeeRate min_relay_fee{1000};          // 1 unit per byte, as chosen for the testnet
    Amount dust_limit = 420;              // smaller outputs are refused, see below
    std::size_t max_tx_size = 100'000;    // bytes, like Bitcoin's 100 kvB
    std::size_t max_bytes = 50'000'000;   // the serialized size of everything in the mempool
    std::int64_t expiry = 14 * 24 * 3600;  // seconds a transaction may wait (2 weeks, as in Bitcoin)
    std::size_t ancestor_limit = 25;      // a transaction plus its unconfirmed ancestors
    std::size_t descendant_limit = 25;    // a transaction plus its unconfirmed descendants
    std::size_t max_replaced = 100;       // transactions one replacement may evict
};
// The dust limit: spending an output means a 100-byte input (outpoint 36 + signature 64) on top
// of its own 40 bytes. At 3 units per byte, Bitcoin's dust relay fee, that's 420 units: an output
// worth less would cost over a third of its value to spend.

enum class MempoolResult {
    Accepted,
    Duplicate,      // already here (same txid)
    Invalid,        // breaks a consensus rule: a coinbase, check_transaction, outputs over inputs,
                    // a bad signature
    NonStandard,    // valid, but policy refuses it: version, size, a dust output
    MissingInputs,  // spends an output that doesn't exist (yet), or one already spent in the chain
    Premature,      // spends a coinbase output that hasn't matured
    FeeTooLow,      // pays less than the minimum relay fee rate
    Conflict,       // spends what a waiting transaction spends, without paying enough to replace it
    TooLongChain,   // would break the ancestor or descendant limit
    MempoolFull,    // pays too little to stay in the full mempool
};

struct MempoolAccept {
    MempoolResult result = MempoolResult::Accepted;
    std::string reason;             // why not, in a few words; empty when accepted
    Amount fee = 0;                 // when accepted
    std::vector<uint256> replaced;  // txids evicted by this replacement, descendants included

    bool accepted() const { return result == MempoolResult::Accepted; }
};

struct MempoolEntry {
    Transaction tx;
    uint256 txid;
    uint256 wtxid;
    Amount fee = 0;
    std::size_t size = 0;        // serialized bytes
    std::int64_t time = 0;       // when it arrived
    std::uint64_t sequence = 0;  // arrival order; a parent always arrives before its children
    std::set<uint256> parents;   // waiting transactions it spends outputs of
    std::set<uint256> children;  // waiting transactions spending its outputs
};

// Told about transactions entering the mempool (the wallet, to see incoming payments).
class MempoolListener {
public:
    virtual ~MempoolListener() = default;
    virtual void tx_added(const Transaction& tx, std::int64_t time) = 0;
};

// Not thread-safe: use it from the thread that owns the chainstate.
class Mempool final : public ChainListener {
public:
    // Follows `chain` (as a ChainListener) for as long as it exists; `chain` must outlive it.
    explicit Mempool(Chainstate& chain, MempoolOptions options = {});
    ~Mempool() override;
    Mempool(const Mempool&) = delete;
    Mempool& operator=(const Mempool&) = delete;

    // Validates `tx` as a spend in the next block, plus the relay policy, and adds it. The checks
    // run cheapest first; signatures come last. A transaction spending what a waiting one spends
    // replaces it (full replace-by-fee) when it pays a higher fee rate than each one it conflicts
    // with, and a fee covering everything it evicts plus the minimum relay fee for its own size.
    MempoolAccept accept(const Transaction& tx, std::int64_t now);

    const MempoolEntry* find(const uint256& txid) const;
    const MempoolEntry* find_wtxid(const uint256& wtxid) const;
    bool contains(const uint256& txid) const { return find(txid) != nullptr; }
    // The waiting transaction that spends `op`, if any.
    const MempoolEntry* spender(const OutPoint& op) const;
    // The transaction and every waiting one built on it, parents first.
    std::vector<const MempoolEntry*> with_descendants(const uint256& txid) const;

    std::size_t size() const { return entries_.size(); }
    std::size_t bytes() const { return bytes_; }
    Amount total_fee() const { return fees_; }
    std::vector<const MempoolEntry*> entries() const;  // in arrival order: parents before children

    // What a block should take, at most max_bytes of it: the package (a transaction plus its
    // waiting ancestors) with the highest fee rate first, so a child can pay for its parent
    // (Bitcoin's ancestor-feerate selection). Parents always come before their children.
    struct Selection {
        std::vector<Transaction> txs;
        std::size_t bytes = 0;
        Amount fees = 0;
        FeeRate lowest;   // the lowest package rate taken
        bool all = true;  // everything fitted
    };
    Selection select(std::size_t max_bytes) const;
    // The fee rate a new transaction needs to make a block of max_bytes, judged by what's waiting:
    // the minimum relay fee if everything fits, else 1 unit per byte above the lowest rate that
    // still made it.
    FeeRate next_block_fee_rate(std::size_t max_bytes) const;

    // Drops transactions that have waited longer than options().expiry, with their descendants.
    // Returns how many went.
    std::size_t expire(std::int64_t now);

    void add_listener(MempoolListener& listener) { listeners_.push_back(&listener); }
    void remove_listener(MempoolListener& listener);
    const MempoolOptions& options() const { return options_; }

    // ChainListener: a connected block takes its transactions out, and any that conflict with them
    // (with their descendants). A reorganisation puts the disconnected blocks' transactions back,
    // where they're still valid.
    void block_connected(const Block& block, const BlockIndex& node) override;
    void block_disconnected(const Block& block, const BlockIndex& node) override;
    void tip_changed(const BlockIndex& tip) override;

private:
    MempoolAccept add(const Transaction& tx, std::int64_t time, bool notify);
    std::set<uint256> descendants_of(const std::set<uint256>& roots) const;  // roots included
    std::set<uint256> ancestors_of(const std::set<uint256>& parents) const;  // parents included
    // By value: callers pass ids that live in the entries being removed.
    void remove_one(uint256 txid);
    void remove_with_descendants(uint256 txid);
    void trim();

    Chainstate& chain_;
    MempoolOptions options_;
    std::unordered_map<uint256, MempoolEntry> entries_;  // by txid
    std::unordered_map<uint256, uint256> by_wtxid_;      // wtxid -> txid
    std::unordered_map<OutPoint, uint256> spenders_;     // outpoint -> txid spending it
    std::size_t bytes_ = 0;
    Amount fees_ = 0;
    std::uint64_t next_sequence_ = 0;
    std::vector<std::vector<Transaction>> disconnected_;  // per disconnected block, newest first
    std::vector<MempoolListener*> listeners_;
};

const char* to_string(MempoolResult r);

}  // namespace bound
