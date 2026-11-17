#pragma once

#include "core/amount.h"
#include "core/block.h"
#include "core/uint256.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string_view>

namespace bound {

// Milestone 15: making a network's genesis block, as launch day does for mainnet (bound-genesis).
//
// The genesis coinbase is Bitcoin's: no inputs, the first block's reward paid to a key nobody
// holds (all zeros; the chain never adds it to the UTXO set either), and a message in the memo.
// Bitcoin's quoted that day's newspaper, "The Times 03/Jan/2009 Chancellor on brink of second
// bailout for banks", which showed nobody could have mined the chain before that day.

// The unmined genesis block: that coinbase, then a header with version 1, no parent, its merkle
// root, `time`, `bits` and nonce 0. nullopt if the message is longer than a memo may be
// (Transaction::kMaxMemo bytes).
std::optional<Block> make_genesis(std::string_view message, Amount reward, std::uint32_t time, std::uint32_t bits);

struct ConsensusParams;

// Mines a genesis header on `threads` threads: every nonce at header.time, then every nonce a
// second later, and so on, until its proof of work (BoundHash, with params.pow_lines) meets
// header.bits. A network's easiest target takes millions of hashes, far fewer than a second's 2^32
// nonces, so in practice the time never moves on; the search would still be exact if it did.
// `progress` (optional) is called from one thread about once a second with the hashes tried so
// far. nullopt if `stop` is requested first, or if the bits ask for a target above
// params.pow_limit or none at all. `nonces_per_second` is how many nonces are tried at each time
// before the next second (every one of them unless a test asks for fewer).
std::optional<BlockHeader> mine_genesis(BlockHeader header, const ConsensusParams& params, unsigned threads,
                                        std::stop_token stop = {},
                                        const std::function<void(std::uint64_t hashes)>& progress = {},
                                        std::uint64_t nonces_per_second = std::uint64_t{1} << 32);

}  // namespace bound
