#include "core/genesis.h"

#include "core/miner.h"
#include "core/params.h"
#include "core/pow.h"
#include "core/transaction.h"
#include "crypto/ed25519.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace bound {

std::optional<Block> make_genesis(std::string_view message, Amount reward, std::uint32_t time, std::uint32_t bits) {
    if (message.size() > Transaction::kMaxMemo) return std::nullopt;
    const Bytes memo(message.begin(), message.end());
    Block block;
    block.txs.push_back(make_coinbase(0, reward, PublicKey{}, memo));
    block.header.version = 1;
    block.header.merkle_root = block.compute_merkle_root();
    block.header.time = time;
    block.header.bits = bits;
    block.header.nonce = 0;
    return block;
}

// The search space is cut into chunks of 256 nonces (about a quarter of a second of BoundHash),
// handed out in order: with every nonce tried at each time, chunk c is the nonces (c mod 2^24) * 256
// onwards, at header.time + c / 2^24. Once a thread finds a solution in chunk c, nobody starts a
// chunk after c, but the chunks before it still finish. So the answer is always the first solution
// in that order, however many threads search: anyone can run it again and get the same block.
std::optional<BlockHeader> mine_genesis(BlockHeader header, const ConsensusParams& params, unsigned threads,
                                        std::stop_token stop,
                                        const std::function<void(std::uint64_t hashes)>& progress,
                                        std::uint64_t nonces_per_second) {
    const std::optional<uint256> target = target_from_compact(header.bits);
    if (!target || target->is_zero() || *target > params.pow_limit) return std::nullopt;
    nonces_per_second = std::clamp<std::uint64_t>(nonces_per_second, 1, std::uint64_t{1} << 32);

    const std::uint64_t chunk_size = std::min(std::uint64_t{256}, nonces_per_second);
    const std::uint64_t chunks_per_second = (nonces_per_second + chunk_size - 1) / chunk_size;
    const std::uint64_t last_chunk =
        (std::uint64_t{std::numeric_limits<std::uint32_t>::max()} - header.time + 1) * chunks_per_second - 1;

    std::atomic<std::uint64_t> next_chunk{0};
    std::atomic<std::uint64_t> best_chunk{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> hashes{0};
    std::mutex mutex;
    std::optional<BlockHeader> best;

    const auto search = [&](bool reports) {
        auto last_report = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            const std::uint64_t chunk = next_chunk.fetch_add(1);
            if (chunk > last_chunk || chunk > best_chunk.load()) return;
            BlockHeader h = header;
            h.time = static_cast<std::uint32_t>(header.time + chunk / chunks_per_second);
            const std::uint64_t first = (chunk % chunks_per_second) * chunk_size;
            const auto start = static_cast<std::uint32_t>(first);
            const std::uint64_t tries = std::min(chunk_size, nonces_per_second - first);
            const std::optional<BlockHeader> solved = mine(h, params, start, tries);
            hashes += solved ? std::uint64_t{solved->nonce} - start + 1 : tries;
            if (solved) {
                const std::lock_guard lock(mutex);
                if (chunk < best_chunk.load()) {
                    best_chunk = chunk;
                    best = solved;
                }
            }
            if (reports && progress) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_report >= std::chrono::seconds(1)) {
                    last_report = now;
                    progress(hashes.load());
                }
            }
        }
    };

    std::vector<std::jthread> workers;
    const unsigned count = std::max(1u, threads);
    workers.reserve(count - 1);
    for (unsigned i = 1; i < count; ++i) workers.emplace_back([&] { search(false); });
    search(true);  // this thread is one of them, and the one that reports
    for (std::jthread& w : workers) w.join();

    if (stop.stop_requested()) return std::nullopt;  // chunks before the best may not have finished
    return best;
}

}  // namespace bound
