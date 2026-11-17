#include "core/consensus.h"
#include "core/miner.h"
#include "core/pow.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <utility>
#include <vector>

using namespace bound;

namespace {

ConsensusParams economics() {
    ConsensusParams p;
    p.initial_subsidy = 5 * kCoin;
    p.halving_interval = 2'100'000;
    return p;
}

Transaction coinbase(int height = 1) {
    Transaction tx;
    tx.outputs = {{5 * kCoin, test_key("alice").public_key()}};
    tx.lock_time = static_cast<std::uint32_t>(height);
    return tx;
}

Transaction signed_payment() {
    Transaction tx = rent_payment();
    sign_all_inputs(tx, test_key("alice"));
    return tx;
}

}  // namespace

TEST(Subsidy, Schedule) {
    const ConsensusParams p = economics();
    EXPECT_EQ(block_subsidy(0, p), 500'000'000);
    EXPECT_EQ(block_subsidy(2'099'999, p), 500'000'000);
    EXPECT_EQ(block_subsidy(2'100'000, p), 250'000'000);
    EXPECT_EQ(block_subsidy(4'200'000, p), 125'000'000);
    EXPECT_EQ(block_subsidy(28 * 2'100'000, p), 1);  // the last base unit of subsidy
    EXPECT_EQ(block_subsidy(29 * 2'100'000, p), 0);
}

// A bare `initial >> (height / interval)` shifts by 64 here. That's undefined behaviour, and on
// x64 the shift count wraps mod 64, so the full reward comes back.
TEST(Subsidy, ZeroFromThe64thHalving) {
    const ConsensusParams p = economics();
    EXPECT_EQ(block_subsidy(64 * 2'100'000, p), 0);
    EXPECT_EQ(block_subsidy(65 * 2'100'000, p), 0);
    EXPECT_EQ(block_subsidy(INT_MAX, p), 0);
}

TEST(Subsidy, TotalSupply) {
    const ConsensusParams p = economics();
    Amount total = 0;
    for (int halving = 0; halving < 64; ++halving) {
        total += Amount{p.halving_interval} * block_subsidy(halving * p.halving_interval, p);
    }
    EXPECT_EQ(total, 2'099'999'972'700'000);  // 20,999,999.727 BOUND
    EXPECT_TRUE(money_range(total));
}

TEST(CheckTransaction, AcceptsWellFormed) {
    EXPECT_EQ(check_transaction(signed_payment()), TxCheck::Ok);
    EXPECT_EQ(check_transaction(coinbase()), TxCheck::Ok);
}

TEST(CheckTransaction, NeedsAnOutput) {
    Transaction tx = signed_payment();
    tx.outputs.clear();
    EXPECT_EQ(check_transaction(tx), TxCheck::NoOutputs);
}

TEST(CheckTransaction, OutputAmountRange) {
    Transaction tx = coinbase();
    tx.outputs[0].amount = -1;
    EXPECT_EQ(check_transaction(tx), TxCheck::OutputOutOfRange);
    tx.outputs[0].amount = kMaxMoney + 1;
    EXPECT_EQ(check_transaction(tx), TxCheck::OutputOutOfRange);
    tx.outputs[0].amount = kMaxMoney;
    EXPECT_EQ(check_transaction(tx), TxCheck::Ok);
    tx.outputs[0].amount = 0;  // zero-value outputs are legal
    EXPECT_EQ(check_transaction(tx), TxCheck::Ok);
}

TEST(CheckTransaction, TotalAmountRange) {
    Transaction tx = coinbase();
    tx.outputs = {{kMaxMoney, test_key("alice").public_key()}, {1, test_key("bob").public_key()}};
    EXPECT_EQ(check_transaction(tx), TxCheck::TotalOutOfRange);
}

TEST(CheckTransaction, RejectsDuplicateInputs) {
    Transaction tx = signed_payment();
    tx.inputs[1] = tx.inputs[0];
    EXPECT_EQ(check_transaction(tx), TxCheck::DuplicateInput);
}

TEST(CheckTransaction, MemoLimit) {
    Transaction tx = coinbase();
    tx.memo = Bytes(Transaction::kMaxMemo, 'x');
    EXPECT_EQ(check_transaction(tx), TxCheck::Ok);
    tx.memo.push_back('x');
    EXPECT_EQ(check_transaction(tx), TxCheck::MemoTooLong);
}

TEST(CheckTransaction, OneSignaturePerInput) {
    EXPECT_EQ(check_transaction(rent_payment()), TxCheck::SignatureCount);  // unsigned
    Transaction cb = coinbase();
    cb.signatures.push_back(Signature{});
    EXPECT_EQ(check_transaction(cb), TxCheck::SignatureCount);
}

TEST(CheckTransaction, LockTimeIsReservedOutsideCoinbases) {
    Transaction tx = signed_payment();
    tx.lock_time = 5;
    EXPECT_EQ(check_transaction(tx), TxCheck::BadLockTime);
    EXPECT_EQ(check_transaction(coinbase(5)), TxCheck::Ok);
}

TEST(CheckTransaction, ReportsTheFirstFailureInOrder) {
    Transaction tx = rent_payment();  // unsigned, so SignatureCount would also fire
    tx.inputs[1] = tx.inputs[0];
    EXPECT_EQ(check_transaction(tx), TxCheck::DuplicateInput);
    tx.outputs.clear();
    tx.lock_time = 5;
    EXPECT_EQ(check_transaction(tx), TxCheck::NoOutputs);
}

namespace {

ConsensusParams block_params() {
    ConsensusParams p;
    p.pow_limit_bits = 0x207fffff;  // about 1 in 2 hashes qualifies
    p.pow_limit = target_from_compact(p.pow_limit_bits).value();
    p.max_block_size = 1'000'000;
    return p;
}

// Optionally recomputes the merkle root, then mines the header.
Block seal(Block b, const ConsensusParams& p, bool fix_merkle_root = true) {
    if (fix_merkle_root) b.header.merkle_root = b.compute_merkle_root();
    b.header = mine(b.header, p).value();
    return b;
}

Block with_txs(std::vector<Transaction> txs) {
    Block b;
    b.header.prev_hash = test_hash("parent");
    b.header.time = 1'800'000'000;
    b.header.bits = 0x207fffff;
    b.txs = std::move(txs);
    return b;
}

Block valid_block(const ConsensusParams& p) { return seal(with_txs({coinbase(), signed_payment()}), p); }

}  // namespace

TEST(CheckBlock, AcceptsAValidBlock) {
    const ConsensusParams p = block_params();
    EXPECT_EQ(check_block(valid_block(p), p), BlockCheck::Ok);
}

TEST(CheckBlock, RejectsBadProofOfWork) {
    const ConsensusParams p = block_params();
    Block b = valid_block(p);
    while (check_proof_of_work(b.header, p)) ++b.header.nonce;
    EXPECT_EQ(check_block(b, p), BlockCheck::BadProofOfWork);
}

TEST(CheckBlock, ChecksProofOfWorkFirst) {
    const ConsensusParams p = block_params();
    Block b = with_txs({});
    while (check_proof_of_work(b.header, p)) ++b.header.nonce;
    EXPECT_EQ(check_block(b, p), BlockCheck::BadProofOfWork);  // not NoTransactions
}

TEST(CheckBlock, RejectsAnEmptyBlock) {
    const ConsensusParams p = block_params();
    EXPECT_EQ(check_block(seal(with_txs({}), p), p), BlockCheck::NoTransactions);
}

TEST(CheckBlock, RequiresACoinbaseFirst) {
    const ConsensusParams p = block_params();
    EXPECT_EQ(check_block(seal(with_txs({signed_payment()}), p), p), BlockCheck::FirstNotCoinbase);
}

TEST(CheckBlock, RejectsASecondCoinbase) {
    const ConsensusParams p = block_params();
    EXPECT_EQ(check_block(seal(with_txs({coinbase(1), coinbase(2)}), p), p), BlockCheck::ExtraCoinbase);
}

TEST(CheckBlock, RejectsABadTransaction) {
    const ConsensusParams p = block_params();
    Transaction no_outputs = signed_payment();
    no_outputs.outputs.clear();
    EXPECT_EQ(check_block(seal(with_txs({coinbase(), no_outputs}), p), p), BlockCheck::BadTransaction);
}

// An unsigned transaction can't even be serialized, so this only works if BadTransaction is
// checked before anything that computes wtxids or the block size.
TEST(CheckBlock, ChecksTransactionsBeforeSerializing) {
    const ConsensusParams p = block_params();
    Block b = with_txs({coinbase(), rent_payment()});
    b.header.merkle_root = test_hash("anything");
    EXPECT_EQ(check_block(seal(b, p, false), p), BlockCheck::BadTransaction);
}

TEST(CheckBlock, RejectsAWrongMerkleRoot) {
    const ConsensusParams p = block_params();
    Block b = valid_block(p);
    b.header.merkle_root = test_hash("wrong");
    EXPECT_EQ(check_block(seal(b, p, false), p), BlockCheck::BadMerkleRoot);
}

// The merkle tree commits to wtxids, so a relaying peer can't swap in a different signature
// without changing the block hash.
TEST(CheckBlock, MerkleRootCoversSignatures) {
    const ConsensusParams p = block_params();
    Block b = valid_block(p);
    b.txs[1].signatures[0][0] ^= 0x01;
    EXPECT_EQ(check_block(seal(b, p, false), p), BlockCheck::BadMerkleRoot);
}

TEST(CheckBlock, EnforcesMaxBlockSize) {
    ConsensusParams p = block_params();
    const Block b = valid_block(p);
    p.max_block_size = b.serialize().size();
    EXPECT_EQ(check_block(b, p), BlockCheck::Ok);
    p.max_block_size -= 1;
    EXPECT_EQ(check_block(b, p), BlockCheck::TooLarge);
}
