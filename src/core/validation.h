#pragma once

#include "core/amount.h"
#include "core/block.h"
#include "core/chain.h"
#include "core/coins.h"
#include "core/params.h"
#include "core/storage.h"
#include "core/transaction.h"
#include "core/uint256.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace bound {

// Milestone 3: the rules that need the UTXO set (Bitcoin's consensus/tx_verify.cpp and the
// ConnectBlock/DisconnectBlock half of validation.cpp).

enum class InputsCheck {
    Ok,
    MissingInput,          // an input isn't in the UTXO set: it never existed, or it's spent
    ImmatureCoinbase,      // spends a coinbase output less than coinbase_maturity blocks old
    InputValueOutOfRange,  // the running total of input values leaves money_range
    InputsBelowOutputs,    // the outputs pay more than the inputs bring in
    BadSignature,          // a signature isn't by the spent output's recipient
};

// Can non-coinbase `tx` (which already passed check_transaction) spend its inputs in a block at
// `spend_height`? Checks in the order listed; signatures come last because they cost the most.
// On Ok, sets *fee (if fee isn't null) to sum(inputs) - sum(outputs).
InputsCheck check_tx_inputs(const Transaction& tx, const UtxoSet& utxos, int spend_height,
                            const ConsensusParams& params, Amount* fee);

// Applies `block` at `height` (>= 1) to `utxos`: spends every input and adds every output.
// Transactions are applied in order, so one may spend an output created earlier in the same
// block. On Ok, `undo` holds what disconnect_block needs. On failure `utxos` is left exactly as
// it was. (ConnectResult is in chain.h.) Milestone 16: check_block's rules are checked without
// proof of work, which the header passed when it entered the block tree.
ConnectResult connect_block(const Block& block, int height, UtxoSet& utxos, BlockUndo& undo,
                            const ConsensusParams& params);

// Reverses connect_block. Precondition: `block` is the last block connected to `utxos`, and
// `undo` is what connecting it produced.
void disconnect_block(const Block& block, const BlockUndo& undo, UtxoSet& utxos);

struct BlockAcceptResult {
    AcceptResult header = AcceptResult::Accepted;  // the header rules; InvalidParent also when an
                                                   // ancestor turned out invalid while connecting
    ConnectResult body = ConnectResult::Ok;        // BadBlock (check_block), or why this block
                                                   // failed to connect

    bool accepted() const { return header == AcceptResult::Accepted && body == ConnectResult::Ok; }
};

// Milestone 7: told about every change to the active chain as it happens (Bitcoin's
// CValidationInterface), on the thread that's changing it. A reorganisation disconnects blocks
// back to the fork first, newest first, then connects the new branch; tip_changed comes once at
// the end. Listeners may read the chainstate but must not change it.
class ChainListener {
public:
    virtual ~ChainListener() = default;
    // The active chain gained `block`, which is now its tip at node.height.
    virtual void block_connected(const Block&, const BlockIndex&) {}
    // The active chain lost its tip `block`; node.parent is the tip now.
    virtual void block_disconnected(const Block&, const BlockIndex&) {}
    // The active chain settled after one or more of the calls above.
    virtual void tip_changed(const BlockIndex&) {}
};

// Milestone 4: the node's validated state. A tree of every valid header it has seen, the active
// chain through it (the valid chain with the most work; of equal ones, the one complete first),
// and the UTXO set at the active tip.
//
// Milestone 5: all of it lives in a KeyValueStore and a BlockStore, and every change is one
// atomic write, so whatever moment a crash comes, the stored tip and the stored UTXO set agree.
// A reorganisation is many writes; if it's cut short, the next open finishes it.
class Chainstate {
public:
    // Only in memory, gone when destroyed (tests, `boundd demo`). Starts at genesis; the genesis
    // coinbase never enters the UTXO set, so it can't be spent. Keeps a reference to params,
    // which must outlive the chainstate.
    explicit Chainstate(const ConsensusParams& params);
    // On storage that outlives the chainstate. Empty storage starts at genesis. Otherwise it loads
    // the block tree, the tip and the UTXO set, checks the last few blocks against their index
    // entries and undo data, and finishes any reorganisation a crash interrupted. Throws
    // std::runtime_error if the storage holds another network's chain or is damaged.
    // If a storage call throws later on, this object no longer matches what's stored: drop it and
    // open a new one, which carries on from the last complete write.
    Chainstate(const ConsensusParams& params, KeyValueStore& db, BlockStore& blocks);
    // The tree is linked by pointers into this object, so it can't be copied.
    Chainstate(const Chainstate&) = delete;
    Chainstate& operator=(const Chainstate&) = delete;

    // Adds a header without its transactions (headers-first sync, for milestone 6's networking).
    // Checks the rules in the order AcceptResult lists them. Proof of work comes before every check
    // that needs the tree: whoever floods us with headers must do that work for each one, and a
    // header without it costs its sender the connection. (Milestone 16: BoundHash makes it about a
    // millisecond for us too, so a duplicate is still spotted first, by its id.)
    // Never changes the active chain. `now` is the current Unix time, passed in so tests control
    // the clock.
    AcceptResult accept_header(const BlockHeader& header, std::int64_t now);

    // Accepts a block on any branch: the header rules (skipped if accept_header already ran),
    // check_block, then it's stored and the tip moves to the valid chain with the most work,
    // disconnecting and connecting blocks as needed. accepted() means valid as far as checked:
    // connected if it's now on the active chain, only stored if its branch has less work. A block
    // that fails to connect, and everything built on it, is remembered as invalid.
    BlockAcceptResult accept_block(const Block& block, std::int64_t now);

    // The active chain.
    int height() const { return tip().height; }
    const BlockIndex& tip() const { return *active_.back(); }
    const BlockIndex& at(int height) const;  // precondition: 0 <= height <= height()
    Block block_at(int height) const;        // read from the block store; same precondition
    bool in_active_chain(const BlockIndex& node) const;

    // Any block in the tree, on any branch; nullptr if unknown.
    const BlockIndex* find(const uint256& hash) const;
    // Headers in the tree, on every branch, with or without their transactions.
    std::size_t tree_size() const { return tree_.size(); }
    // Milestone 6: the valid header with the most work, whether or not its transactions are here:
    // where syncing is heading. Never has less work than the tip.
    const BlockIndex& best_header() const { return *best_header_; }
    // A stored block, from any branch. Precondition: node.have_data.
    Block read_block(const BlockIndex& node) const;
    // Milestone 8: the coins a block spent, in the order it spent them. There for every block that
    // has ever been on the active chain (genesis excepted), whether or not it still is; nullopt
    // for a block that was never connected. For indexes and for showing a transaction's inputs.
    std::optional<BlockUndo> read_undo(const BlockIndex& node) const;

    const UtxoSet& utxos() const { return utxos_; }
    const ConsensusParams& params() const { return params_; }

    // Milestone 7. Called in the order added. A listener must be removed before it's destroyed.
    void add_listener(ChainListener& listener);
    void remove_listener(ChainListener& listener);

private:
    // Orders tip candidates best first: most work, then linked earliest.
    struct MoreWork {
        bool operator()(const BlockIndex* a, const BlockIndex* b) const;
    };

    void open();
    void create();
    void load();
    void verify_recent_blocks(int count) const;
    void find_best_header();

    AcceptResult check_header(const BlockHeader& header, std::int64_t now) const;
    // These change the tree in memory and add the matching records to `batch`.
    BlockIndex& add_header(const BlockHeader& header, WriteBatch& batch);
    void link(BlockIndex& node, WriteBatch& batch);
    void mark_invalid(BlockIndex& node, ConnectResult reason, WriteBatch& batch);
    // These write their own batches.
    void activate_best_chain();
    bool connect_tip(BlockIndex& node);
    void disconnect_tip();

    const ConsensusParams& params_;
    std::unique_ptr<KeyValueStore> owned_db_;   // only for the in-memory constructor
    std::unique_ptr<BlockStore> owned_blocks_;  // likewise
    KeyValueStore& db_;
    BlockStore& block_store_;
    std::unordered_map<uint256, BlockIndex> tree_;  // every accepted header, by hash
    std::vector<BlockIndex*> active_;               // active_[h] is the active chain's block at height h
    std::set<BlockIndex*, MoreWork> candidates_;    // linked, valid blocks; the tip belongs at begin()
    BlockIndex* best_header_ = nullptr;
    std::uint64_t next_sequence_ = 0;
    UtxoSet utxos_;
    std::vector<ChainListener*> listeners_;
    bool chain_changed_ = false;  // set by connect_tip and disconnect_tip, for tip_changed
};

}  // namespace bound
