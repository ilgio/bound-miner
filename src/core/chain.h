#pragma once

#include "core/arith_uint256.h"
#include "core/block.h"
#include "core/uint256.h"

#include <cstdint>
#include <vector>

namespace bound {

// Why a header can't join the block tree (Chainstate::accept_header, and the header half of
// accept_block). Checked in this order; the first failure wins.
enum class AcceptResult {
    Accepted,
    Duplicate,       // already in the tree
    BadProofOfWork,  // check_proof_of_work(header, params) fails
    UnknownParent,   // prev_hash isn't in the tree (yet)
    InvalidParent,   // the parent, or one of its ancestors, failed validation
    BadDifficulty,   // bits != next_work_required(parent)
    TimeTooOld,      // time <= the parent's median time past
    TimeTooNew,      // time > now + params.max_future_block_time; may be fine later
};

// Why a block can't be connected on top of its parent (connect_block in validation.h). It lives
// here because the block tree remembers why a block failed.
enum class ConnectResult {
    Ok,
    BadBlock,           // fails check_block (the context-free rules)
    BadCoinbaseHeight,  // coinbase lock_time != height
    BadInputs,          // a transaction fails check_tx_inputs (this catches double spends too)
    DuplicateOutput,    // would overwrite an unspent output (Bitcoin's BIP30; needs a hash collision here)
    CoinbaseTooLarge,   // the coinbase pays more than block_subsidy(height) + the block's fees
};

// Milestone 5: where a BlockStore keeps a block's bytes.
struct BlockPos {
    std::uint32_t file = 0;
    std::uint32_t offset = 0;

    friend bool operator==(const BlockPos&, const BlockPos&) = default;
};

// Milestone 4: a node of the block tree (Bitcoin's CBlockIndex). Every header that passed the
// header rules has one, whether or not it's on the active chain or its transactions have arrived.
struct BlockIndex {
    uint256 hash;
    BlockHeader header;
    BlockIndex* parent = nullptr;  // null only for genesis
    std::vector<BlockIndex*> children;
    int height = 0;
    arith_uint256 chain_work;  // block_proof(bits) summed over this block and all its ancestors

    bool have_data = false;      // its transactions are stored, at `pos`
    BlockPos pos;
    bool linked = false;         // ... and so are every ancestor's, so the chain up to it can be connected
    std::uint64_t sequence = 0;  // order of becoming linked: of two chains with equal work, the earlier wins
    bool invalid = false;        // it or an ancestor failed validation; it will never be connected
    ConnectResult invalid_reason = ConnectResult::Ok;  // why it failed itself; Ok if only an ancestor did

    // The ancestor at height h (this block itself when h == height). Precondition: 0 <= h <= height.
    const BlockIndex* ancestor(int h) const;
    // Median of the times of the last `span` blocks ending here (all of them near genesis). For an
    // even count, the upper middle (sorted[n / 2]), like Bitcoin's GetMedianTimePast.
    std::uint32_t median_time_past(int span) const;
};

// Milestone 6: Bitcoin's block locator, a short description of the chain ending at `tip` for a
// peer to find where its chain and ours part: the hashes of the last 10 blocks, then exponentially
// further apart, ending with genesis (about 30 hashes for a million blocks).
std::vector<uint256> block_locator(const BlockIndex& tip);

}  // namespace bound
