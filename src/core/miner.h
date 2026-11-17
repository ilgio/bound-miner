#pragma once

#include "core/amount.h"
#include "core/block.h"
#include "core/transaction.h"
#include "core/uint256.h"
#include "crypto/ed25519.h"
#include "util/bytes.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace bound {

class Chainstate;
struct ConsensusParams;

// Tries nonces start_nonce, start_nonce + 1, ... in order until check_proof_of_work(header,
// params) holds (milestone 16: BoundHash), and returns the header with that nonce.
// Gives up and returns nullopt after max_tries attempts or after trying nonce 0xffffffff (it
// never wraps to 0); the caller then changes another field (time now, the coinbase extra-nonce
// from milestone 2) and calls again. Only `nonce` is modified.
std::optional<BlockHeader> mine(BlockHeader header, const ConsensusParams& params,
                                std::uint32_t start_nonce = 0,
                                std::uint64_t max_tries = std::uint64_t{1} << 32);

// Milestone 2. The coinbase for a block at `height`: no inputs, one output paying `reward` to
// `recipient`, lock_time = height, the given memo, no signatures. Precondition: height >= 0.
Transaction make_coinbase(int height, Amount reward, const PublicKey& recipient, ByteSpan memo);

// Milestone 3. An unmined block on top of chain's tip: a coinbase paying `recipient` the subsidy
// plus the fees of `txs`, then `txs` in order (later ones may spend earlier ones). The coinbase
// memo is an 8-byte extra nonce, starting at 0. The header gets the tip as prev_hash, the merkle
// root, the bits next_work_required demands and max(time, the tip's median time past + 1).
// nullopt if any of `txs` is a coinbase or isn't valid at that point (check_transaction, then
// check_tx_inputs).
std::optional<Block> make_block_template(const Chainstate& chain, const PublicKey& recipient,
                                         std::vector<Transaction> txs, std::uint32_t time);

// Mines a template: searches the whole nonce range, and each time it runs out, increments the
// coinbase's extra nonce, recomputes the merkle root and searches again. Precondition: txs[0] is a
// coinbase whose memo is an 8-byte extra nonce, and params.pow_limit allows header.bits. Can only
// fail to return if the target is so hard that no hash ever meets it.
Block mine_block(Block block, const ConsensusParams& params);

// Milestone 6: mines on threads of its own while the node runs (one until milestone 12, any number
// since). Templates come from the thread that owns the chainstate, and solved blocks go back to it;
// this class never touches the chain.
class BackgroundMiner {
public:
    // Keeps what it needs of params (the pow limit and BoundHash's size), not a reference.
    explicit BackgroundMiner(const ConsensusParams& params, unsigned threads = 1);
    ~BackgroundMiner();  // stops the threads
    static unsigned default_threads();  // every core but one, which the node keeps
    BackgroundMiner(const BackgroundMiner&) = delete;
    BackgroundMiner& operator=(const BackgroundMiner&) = delete;

    // New work (a template from make_block_template): whatever the miner was on is dropped.
    // nullopt pauses it, e.g. while the node is still catching up.
    void set_work(std::optional<Block> block);
    // A block solved since the last call, if any. After solving, the miner waits for new work.
    std::optional<Block> take_solved();
    std::uint64_t hashes() const { return hashes_; }  // tried so far, by every thread, for a hash rate
    unsigned threads() const { return static_cast<unsigned>(threads_.size()); }

private:
    void run(std::stop_token stop, unsigned index, unsigned count);

    uint256 pow_limit_;
    std::uint32_t pow_lines_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Block> work_;
    std::uint64_t generation_ = 0;  // bumped by set_work
    std::optional<Block> solved_;
    std::uint64_t solved_generation_ = 0;  // the generation solved_ (or a block already taken) solves
    std::atomic<std::uint64_t> hashes_{0};
    std::vector<std::jthread> threads_;  // last, so they start after, and stop before, everything above
};

}  // namespace bound
