#pragma once

#include "core/amount.h"
#include "core/block.h"
#include "core/params.h"
#include "core/transaction.h"

namespace bound {

// New coins the coinbase at `height` may create: params.initial_subsidy halved once every
// params.halving_interval blocks, and 0 from the 64th halving on. (Shifting an int64 by 64 or
// more is undefined behaviour; Bitcoin's GetBlockSubsidy has the same guard.)
Amount block_subsidy(int height, const ConsensusParams& params);

// Context-free transaction rules: everything that can be checked without the UTXO set.
// Signatures are *not* verified here, because that needs the spent outputs' keys (milestone 3).
enum class TxCheck {
    Ok,
    NoOutputs,         // outputs is empty
    OutputOutOfRange,  // an amount fails money_range
    TotalOutOfRange,   // the running total of amounts leaves money_range
    DuplicateInput,    // the same outpoint listed twice
    MemoTooLong,       // memo.size() > Transaction::kMaxMemo
    SignatureCount,    // signatures.size() != inputs.size()
    BadLockTime,       // a non-coinbase with lock_time != 0
};
// Checks in the order listed and returns the first failure. The two amount checks share one pass
// over the outputs, as in Bitcoin: per output, its range, then the running total (which also
// rules out overflow).
TxCheck check_transaction(const Transaction& tx);

enum class BlockCheck {
    Ok,
    BadProofOfWork,    // check_proof_of_work(header, params) fails (BoundHash, milestone 16)
    NoTransactions,
    FirstNotCoinbase,  // txs[0] has inputs
    ExtraCoinbase,     // a coinbase after txs[0]
    BadTransaction,    // some transaction fails check_transaction
    BadMerkleRoot,     // header.merkle_root != compute_merkle_root()
    TooLarge,          // serialize().size() > params.max_block_size
};
// Context-free block rules, in the order listed; the first failure wins. Proof of work comes
// first, as the filter that costs a forger work. BadTransaction comes before BadMerkleRoot and
// TooLarge because those serialize, and serialize() needs every signature count to be right.
// Milestone 16: check_pow = false skips proof of work, for a block whose header already passed it
// (it's in the block tree): BoundHash costs about a millisecond, too much to spend twice. Bitcoin's
// CheckBlock has the same switch.
BlockCheck check_block(const Block& block, const ConsensusParams& params, bool check_pow = true);

}  // namespace bound
