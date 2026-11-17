#pragma once

#include "core/block.h"
#include "core/chain.h"
#include "core/coins.h"
#include "core/consensus.h"
#include "core/mempool.h"
#include "core/miner.h"
#include "core/pow.h"
#include "core/transaction.h"
#include "core/uint256.h"
#include "core/validation.h"
#include "crypto/ed25519.h"
#include "crypto/sha256.h"

#include <cstdint>
#include <filesystem>
#include <ios>
#include <ostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bound {

// GoogleTest finds these by ADL and uses them in failure messages.
inline void PrintTo(const uint256& v, std::ostream* os) { *os << v.to_hex(); }
inline void PrintTo(const arith_uint256& v, std::ostream* os) { *os << v.to_uint256().to_hex(); }

inline void PrintTo(const BlockHeader& h, std::ostream* os) {
    *os << "{version=" << h.version << " prev=" << h.prev_hash.to_hex()
        << " merkle=" << h.merkle_root.to_hex() << " time=" << h.time << " bits=0x" << std::hex
        << h.bits << std::dec << " nonce=" << h.nonce << "}";
}

inline void PrintTo(const Transaction& tx, std::ostream* os) {
    *os << "{txid=" << tx.txid().to_hex() << " inputs=" << tx.inputs.size()
        << " outputs=" << tx.outputs.size() << " signatures=" << tx.signatures.size() << "}";
}

inline void PrintTo(AcceptResult r, std::ostream* os) {
    switch (r) {
    case AcceptResult::Accepted: *os << "Accepted"; return;
    case AcceptResult::Duplicate: *os << "Duplicate"; return;
    case AcceptResult::BadProofOfWork: *os << "BadProofOfWork"; return;
    case AcceptResult::UnknownParent: *os << "UnknownParent"; return;
    case AcceptResult::InvalidParent: *os << "InvalidParent"; return;
    case AcceptResult::BadDifficulty: *os << "BadDifficulty"; return;
    case AcceptResult::TimeTooOld: *os << "TimeTooOld"; return;
    case AcceptResult::TimeTooNew: *os << "TimeTooNew"; return;
    }
    *os << "AcceptResult(" << static_cast<int>(r) << ")";
}

inline void PrintTo(TxCheck r, std::ostream* os) {
    switch (r) {
    case TxCheck::Ok: *os << "Ok"; return;
    case TxCheck::NoOutputs: *os << "NoOutputs"; return;
    case TxCheck::OutputOutOfRange: *os << "OutputOutOfRange"; return;
    case TxCheck::TotalOutOfRange: *os << "TotalOutOfRange"; return;
    case TxCheck::DuplicateInput: *os << "DuplicateInput"; return;
    case TxCheck::MemoTooLong: *os << "MemoTooLong"; return;
    case TxCheck::SignatureCount: *os << "SignatureCount"; return;
    case TxCheck::BadLockTime: *os << "BadLockTime"; return;
    }
    *os << "TxCheck(" << static_cast<int>(r) << ")";
}

inline void PrintTo(BlockCheck r, std::ostream* os) {
    switch (r) {
    case BlockCheck::Ok: *os << "Ok"; return;
    case BlockCheck::BadProofOfWork: *os << "BadProofOfWork"; return;
    case BlockCheck::NoTransactions: *os << "NoTransactions"; return;
    case BlockCheck::FirstNotCoinbase: *os << "FirstNotCoinbase"; return;
    case BlockCheck::ExtraCoinbase: *os << "ExtraCoinbase"; return;
    case BlockCheck::BadTransaction: *os << "BadTransaction"; return;
    case BlockCheck::BadMerkleRoot: *os << "BadMerkleRoot"; return;
    case BlockCheck::TooLarge: *os << "TooLarge"; return;
    }
    *os << "BlockCheck(" << static_cast<int>(r) << ")";
}

// uint256 from display-order hex, as block explorers print it. Test data only: throws on a typo.
inline uint256 U(std::string_view display_hex) { return uint256::from_hex(display_hex).value(); }

// Test data, derived exactly as the Python that computed the expected values derives it.
inline KeyPair test_key(std::string_view name) { return KeyPair::from_seed(sha256(bytes_of(name))); }
inline uint256 test_hash(std::string_view s) { return uint256(tagged_hash("BOUND/test", bytes_of(s))); }

// Unsigned: Alice spends two outputs, paying 1.5 BOUND to Bob and 3.4999 BOUND back to herself.
inline Transaction rent_payment() {
    Transaction tx;
    tx.inputs = {{test_hash("funding tx a"), 0}, {test_hash("funding tx b"), 3}};
    tx.outputs = {{150'000'000, test_key("bob").public_key()}, {349'990'000, test_key("alice").public_key()}};
    tx.memo = Bytes{'r', 'e', 'n', 't'};
    return tx;
}

// Milestone 3.

inline void PrintTo(InputsCheck r, std::ostream* os) {
    switch (r) {
    case InputsCheck::Ok: *os << "Ok"; return;
    case InputsCheck::MissingInput: *os << "MissingInput"; return;
    case InputsCheck::ImmatureCoinbase: *os << "ImmatureCoinbase"; return;
    case InputsCheck::InputValueOutOfRange: *os << "InputValueOutOfRange"; return;
    case InputsCheck::InputsBelowOutputs: *os << "InputsBelowOutputs"; return;
    case InputsCheck::BadSignature: *os << "BadSignature"; return;
    }
    *os << "InputsCheck(" << static_cast<int>(r) << ")";
}

inline void PrintTo(ConnectResult r, std::ostream* os) {
    switch (r) {
    case ConnectResult::Ok: *os << "Ok"; return;
    case ConnectResult::BadBlock: *os << "BadBlock"; return;
    case ConnectResult::BadCoinbaseHeight: *os << "BadCoinbaseHeight"; return;
    case ConnectResult::BadInputs: *os << "BadInputs"; return;
    case ConnectResult::DuplicateOutput: *os << "DuplicateOutput"; return;
    case ConnectResult::CoinbaseTooLarge: *os << "CoinbaseTooLarge"; return;
    }
    *os << "ConnectResult(" << static_cast<int>(r) << ")";
}

inline constexpr std::uint32_t kTestGenesisTime = 1'800'000'000;
inline constexpr std::int64_t kTestNow = kTestGenesisTime + 100'000'000;  // after every test block

// Testnet's rules with trivial proof of work (about 1 in 2 hashes) and its own mined genesis
// block, for tests that build real chains quickly.
inline ConsensusParams easy_params() {
    ConsensusParams p;
    p.network_name = "unittest";
    p.pow_limit_bits = 0x207fffff;
    p.pow_limit = target_from_compact(p.pow_limit_bits).value();
    p.pow_lines = 64;  // milestone 16: regtest's small BoundHash
    p.max_future_block_time = 2 * 60 * 60;
    p.median_time_span = 11;
    p.target_spacing = 60;
    p.initial_subsidy = 5 * kCoin;
    p.halving_interval = 2'100'000;
    p.max_block_size = 1'000'000;
    p.address_version = 0xf0;
    p.coinbase_maturity = 100;
    p.retarget_interval = 1440;
    p.message_start = {0xfa, 0xbf, 0xb5, 0xda};  // Bitcoin regtest's, which a test network can borrow
    p.default_port = 18444;
    p.default_rpc_port = 18443;
    p.genesis_coinbase = make_coinbase(0, 5 * kCoin, PublicKey{}, bytes_of("unit test genesis"));
    p.genesis.merkle_root = genesis_block(p).compute_merkle_root();
    p.genesis.time = kTestGenesisTime;
    p.genesis.bits = p.pow_limit_bits;
    p.genesis = mine(p.genesis, p).value();
    p.genesis_hash = p.genesis.hash();
    return p;
}

// A mined block on chain's tip, built by make_block_template (so `txs` must be valid), one
// minute after the tip in test time.
inline Block next_block(const Chainstate& chain, const PublicKey& miner, std::vector<Transaction> txs = {}) {
    const auto time = static_cast<std::uint32_t>(kTestGenesisTime + 60 * (chain.height() + 1));
    return mine_block(make_block_template(chain, miner, std::move(txs), time).value(), chain.params());
}

// Mines and accepts `count` empty blocks paying `miner`.
inline void mine_blocks(Chainstate& chain, const PublicKey& miner, int count) {
    for (int i = 0; i < count; ++i) {
        if (!chain.accept_block(next_block(chain, miner), kTestNow).accepted()) throw std::logic_error("test block rejected");
    }
}

// Milestone 4. A mined block on `parent`, which may be anywhere in the tree, with the bits the
// rules require. Its coinbase claims exactly the subsidy; `txs` go in unchecked.
inline Block block_on(const BlockIndex& parent, const ConsensusParams& params, const PublicKey& miner,
                      std::uint32_t time, std::vector<Transaction> txs = {}) {
    Block block;
    block.txs.push_back(make_coinbase(parent.height + 1, block_subsidy(parent.height + 1, params), miner, {}));
    for (Transaction& tx : txs) block.txs.push_back(std::move(tx));
    block.header.prev_hash = parent.hash;
    block.header.merkle_root = block.compute_merkle_root();
    block.header.time = time;
    block.header.bits = next_work_required(parent, params);
    block.header = mine(block.header, params).value();
    return block;
}

// Milestone 7.

inline void PrintTo(MempoolResult r, std::ostream* os) { *os << to_string(r); }

// A transaction spending `inputs`, all key's, to `outputs`, signed.
inline Transaction spend(std::vector<OutPoint> inputs, std::vector<TxOut> outputs, const KeyPair& key, Bytes memo = {}) {
    Transaction tx;
    tx.inputs = std::move(inputs);
    tx.outputs = std::move(outputs);
    tx.memo = std::move(memo);
    sign_all_inputs(tx, key);
    return tx;
}

// The coinbase output of the active chain's block at `height`.
inline OutPoint coinbase_at(const Chainstate& chain, int height) { return {chain.block_at(height).txs[0].txid(), 0}; }

// Milestone 5. A new, empty directory under the system temp directory, deleted with everything in
// it when this goes out of scope. Declare it before the stores that use it.
class TempDir {
public:
    TempDir() : path_(std::filesystem::temp_directory_path() / ("bound-test-" + std::to_string(std::random_device{}()))) {
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace bound
