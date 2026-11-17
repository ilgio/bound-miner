#include "core/consensus.h"

#include "core/pow.h"

#include <set>

namespace bound {

Amount block_subsidy(int height, const ConsensusParams& params) {
    const int halvings = height / params.halving_interval;
    if (halvings >= 64) return 0;
    return params.initial_subsidy >> halvings;
}

TxCheck check_transaction(const Transaction& tx) {
    if (tx.outputs.empty()) return TxCheck::NoOutputs;
    Amount total = 0;
    for (const TxOut& out : tx.outputs) {
        if (!money_range(out.amount)) return TxCheck::OutputOutOfRange;
        total += out.amount;
        if (!money_range(total)) return TxCheck::TotalOutOfRange;
    }
    std::set<OutPoint> seen;
    for (const OutPoint& in : tx.inputs) {
        if (!seen.insert(in).second) return TxCheck::DuplicateInput;
    }
    if (tx.memo.size() > Transaction::kMaxMemo) return TxCheck::MemoTooLong;
    if (tx.signatures.size() != tx.inputs.size()) return TxCheck::SignatureCount;
    if (!tx.is_coinbase() && tx.lock_time != 0) return TxCheck::BadLockTime;
    return TxCheck::Ok;
}

BlockCheck check_block(const Block& block, const ConsensusParams& params, bool check_pow) {
    if (check_pow && !check_proof_of_work(block.header, params)) return BlockCheck::BadProofOfWork;
    if (block.txs.empty()) return BlockCheck::NoTransactions;
    if (!block.txs[0].is_coinbase()) return BlockCheck::FirstNotCoinbase;
    for (std::size_t i = 1; i < block.txs.size(); ++i) {
        if (block.txs[i].is_coinbase()) return BlockCheck::ExtraCoinbase;
    }
    for (const Transaction& tx : block.txs) {
        if (check_transaction(tx) != TxCheck::Ok) return BlockCheck::BadTransaction;
    }
    if (block.header.merkle_root != block.compute_merkle_root()) return BlockCheck::BadMerkleRoot;
    if (block.serialize().size() > params.max_block_size) return BlockCheck::TooLarge;
    return BlockCheck::Ok;
}

}  // namespace bound
