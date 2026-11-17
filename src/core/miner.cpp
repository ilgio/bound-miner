#include "core/miner.h"

#include "core/boundhash.h"
#include "core/consensus.h"
#include "core/params.h"
#include "core/pow.h"
#include "core/validation.h"
#include "util/serialize.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace bound {

namespace {

// Milestone 16: every try is a whole BoundHash. The header goes into its seed in one piece, so
// unlike SHA-256d there's no midstate to reuse between nonces: nothing to precompute, nothing a
// specialised machine could skip. What's reused is the scratchpad, which bound_hash keeps per
// thread.
std::optional<BlockHeader> mine_nonces(BlockHeader header, const uint256& pow_limit, std::uint32_t lines,
                                       std::uint32_t start_nonce, std::uint64_t max_tries) {
    const std::optional<uint256> target = target_from_compact(header.bits);
    if (!target || target->is_zero() || *target > pow_limit) return std::nullopt;  // nothing can meet it
    std::array<std::uint8_t, BlockHeader::kSize> bytes = header.serialize();
    static_assert(BlockHeader::kSize == 80);
    std::uint64_t nonce = start_nonce;
    for (std::uint64_t tries = 0; tries < max_tries && nonce <= 0xffffffff; ++tries, ++nonce) {
        const auto n = static_cast<std::uint32_t>(nonce);
        bytes[76] = static_cast<std::uint8_t>(n);
        bytes[77] = static_cast<std::uint8_t>(n >> 8);
        bytes[78] = static_cast<std::uint8_t>(n >> 16);
        bytes[79] = static_cast<std::uint8_t>(n >> 24);
        if (bound_hash(ByteSpan(bytes), lines) <= *target) {
            header.nonce = n;
            return header;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<BlockHeader> mine(BlockHeader header, const ConsensusParams& params, std::uint32_t start_nonce,
                                std::uint64_t max_tries) {
    return mine_nonces(header, params.pow_limit, params.pow_lines, start_nonce, max_tries);
}

Transaction make_coinbase(int height, Amount reward, const PublicKey& recipient, ByteSpan memo) {
    Transaction tx;
    tx.outputs.push_back(TxOut{reward, recipient});
    tx.lock_time = static_cast<std::uint32_t>(height);
    tx.memo.assign(memo.begin(), memo.end());
    return tx;
}

namespace {

Bytes extra_nonce(std::uint64_t n) {
    ByteWriter w;
    w.u64(n);
    return std::move(w).take();
}

}  // namespace

std::optional<Block> make_block_template(const Chainstate& chain, const PublicKey& recipient,
                                         std::vector<Transaction> txs, std::uint32_t time) {
    const ConsensusParams& params = chain.params();
    const BlockIndex& tip = chain.tip();
    const int height = tip.height + 1;

    // Apply the transactions to a copy of the UTXO set, so later ones can spend earlier ones.
    UtxoSet view = chain.utxos();
    Amount fees = 0;
    for (const Transaction& tx : txs) {
        if (tx.is_coinbase() || check_transaction(tx) != TxCheck::Ok) return std::nullopt;
        Amount fee = 0;
        if (check_tx_inputs(tx, view, height, params, &fee) != InputsCheck::Ok) return std::nullopt;
        fees += fee;
        for (const OutPoint& in : tx.inputs) view.spend(in);
        const uint256 txid = tx.txid();
        for (std::uint32_t i = 0; i < tx.outputs.size(); ++i) view.add({txid, i}, Coin{tx.outputs[i], height, false});
    }

    Block block;
    block.txs.reserve(txs.size() + 1);
    block.txs.push_back(make_coinbase(height, block_subsidy(height, params) + fees, recipient, extra_nonce(0)));
    std::move(txs.begin(), txs.end(), std::back_inserter(block.txs));

    block.header.prev_hash = tip.hash;
    block.header.merkle_root = block.compute_merkle_root();
    block.header.time = std::max(time, tip.median_time_past(params.median_time_span) + 1);
    block.header.bits = next_work_required(tip, params);
    return block;
}

Block mine_block(Block block, const ConsensusParams& params) {
    for (std::uint64_t extra = 1;; ++extra) {
        if (const std::optional<BlockHeader> solved = mine(block.header, params)) {
            block.header = *solved;
            return block;
        }
        block.txs[0].memo = extra_nonce(extra);
        block.header.merkle_root = block.compute_merkle_root();
    }
}

BackgroundMiner::BackgroundMiner(const ConsensusParams& params, unsigned threads)
    : pow_limit_(params.pow_limit), pow_lines_(params.pow_lines) {
    const unsigned n = std::max(1u, threads);
    threads_.reserve(n);
    for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this, i, n](std::stop_token stop) { run(stop, i, n); });
}

BackgroundMiner::~BackgroundMiner() {
    for (std::jthread& t : threads_) t.request_stop();
    wake_.notify_all();
    threads_.clear();  // joins them, before the members they use go
}

unsigned BackgroundMiner::default_threads() {
    const unsigned cores = std::thread::hardware_concurrency();
    return cores > 1 ? cores - 1 : 1;  // one left for the node itself
}

void BackgroundMiner::set_work(std::optional<Block> block) {
    {
        const std::lock_guard lock(mutex_);
        work_ = std::move(block);
        ++generation_;
    }
    wake_.notify_all();
}

std::optional<Block> BackgroundMiner::take_solved() {
    const std::lock_guard lock(mutex_);
    return std::exchange(solved_, std::nullopt);
}

// Each of the `count` threads searches its own coinbases: thread `index` uses the extra nonces
// index, index + count, index + 2 * count, ... (the template's 0 is thread 0's first), so no two
// ever hash the same header.
void BackgroundMiner::run(std::stop_token stop, unsigned index, unsigned count) {
    constexpr std::uint64_t kSlice = 64;       // nonces between looks for new work: about 0.1 s of BoundHash
    std::uint64_t mining = 0;                  // the generation `block` came from
    bool idle = true;                          // nothing to mine: none given yet, paused, or solved
    Block block;
    std::uint64_t nonce = 0;
    std::uint64_t extra = 0;
    const auto use_extra = [&] {
        block.txs[0].memo = extra_nonce(extra);
        block.header.merkle_root = block.compute_merkle_root();
    };
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(mutex_);
            if (idle && !wake_.wait(lock, stop, [&] { return generation_ != mining && work_.has_value(); })) return;
            if (generation_ != mining) {
                mining = generation_;
                idle = !work_.has_value();
                if (idle) continue;
                block = *work_;
                nonce = 0;
                extra = index;
                if (extra != 0) use_extra();
            } else if (solved_generation_ == mining) {  // another thread found this one
                idle = true;
                continue;
            }
        }
        const std::uint64_t tries = std::min(kSlice, (std::uint64_t{1} << 32) - nonce);
        const std::optional<BlockHeader> solved =
            mine_nonces(block.header, pow_limit_, pow_lines_, static_cast<std::uint32_t>(nonce), tries);
        hashes_ += solved ? solved->nonce - nonce + 1 : tries;
        if (solved) {
            block.header = *solved;
            const std::lock_guard lock(mutex_);
            if (solved_generation_ != mining && generation_ == mining) {  // the first, and still wanted
                solved_ = block;
                solved_generation_ = mining;
            }
            idle = true;
            continue;
        }
        nonce += tries;
        if (nonce == std::uint64_t{1} << 32) {  // every nonce tried: this thread's next coinbase
            extra += count;
            use_extra();
            nonce = 0;
        }
    }
}

}  // namespace bound
