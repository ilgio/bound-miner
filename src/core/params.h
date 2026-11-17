#pragma once

#include "core/amount.h"
#include "core/arith_uint256.h"
#include "core/block.h"
#include "core/boundhash.h"
#include "core/transaction.h"
#include "core/uint256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bound {

inline constexpr std::string_view kCoinName = "BOUND";
inline constexpr std::string_view kTicker = "BOUND";

// Milestone 10: the bytes in front of the key in an address, which fix its first character and
// keep networks apart. One byte can only fix a digit 2-9 (testnet's 0xf0 gives '9'); two bytes
// can fix a letter (mainnet's 0x46fb gives 'B').
struct AddressVersion {
    std::array<std::uint8_t, 2> bytes{};
    std::uint8_t size = 0;

    constexpr AddressVersion() = default;
    constexpr AddressVersion(std::uint8_t b) : bytes{b, 0}, size(1) {}  // implicit on purpose
    constexpr AddressVersion(std::uint8_t b0, std::uint8_t b1) : bytes{b0, b1}, size(2) {}
    bool operator==(const AddressVersion&) const = default;
};

// Consensus rules that differ per network. Grows every milestone.
struct ConsensusParams {
    std::string_view network_name;
    uint256 pow_limit;                 // easiest allowed target
    std::uint32_t pow_limit_bits = 0;  // compact form of pow_limit; must round-trip exactly.
                                       // The genesis block's bits, where the difficulty starts.
    // Milestone 16: proof of work is BoundHash (core/boundhash.h) with this many 128-byte lines of
    // scratchpad: 512 KiB on mainnet and testnet; regtest's fewer keep tests quick.
    std::uint32_t pow_lines = kBoundHashLines;
    std::int64_t max_future_block_time = 0;  // seconds a header's time may run ahead of `now`
    int median_time_span = 0;                // headers in the median-time-past window
    BlockHeader genesis;
    uint256 genesis_hash;  // genesis.hash() (its id, SHA-256d), hard-coded as a self-check

    // Milestone 2: economics and block bodies.
    std::int64_t target_spacing = 0;   // seconds per block the difficulty aims for
    Amount initial_subsidy = 0;        // coinbase reward at height 0
    int halving_interval = 0;          // blocks between halvings
    std::size_t max_block_size = 0;    // serialized bytes
    AddressVersion address_version;    // Base58Check version of this network's addresses
    Transaction genesis_coinbase;      // the genesis block's only transaction; never spendable

    // Milestone 3: spending.
    int coinbase_maturity = 0;  // blocks a coinbase output must wait before it can be spent

    // Milestone 4: difficulty adjustment.
    int retarget_interval = 0;  // blocks per difficulty period; see next_work_required
    // Milestone 10: every block keeps its parent's difficulty (regtest, for tests on one machine).
    bool pow_no_retargeting = false;
    // Milestone 11: when set, ASERT (Bitcoin Cash's aserti3-2d) sets every block's difficulty
    // instead of the retarget above: the target doubles for every half-life the chain falls behind
    // its schedule (genesis time + target_spacing per block) and halves for every one it runs
    // ahead. The genesis block is the anchor. 0: the retarget rule.
    std::int64_t asert_half_life = 0;

    // Milestone 6: the peer-to-peer network. Not consensus rules, but they differ per network too.
    std::array<std::uint8_t, 4> message_start{};  // the first 4 bytes of every P2P message
    std::uint16_t default_port = 0;               // where nodes listen unless told otherwise
    // Milestone 7: where the node takes JSON-RPC (127.0.0.1 only).
    std::uint16_t default_rpc_port = 0;

    // Milestone 8: host names that lead to nodes, for a node that knows nobody yet ("host" or
    // "host:port"; see net/seeds.h). DNS names answered by a bound-seeder, or always-on nodes.
    std::vector<std::string> seeds;

    // Hardening: the least work a chain must have before a node syncing from scratch stores its
    // headers (Bitcoin's nMinimumChainWork). Not a consensus rule; raise it each release to the
    // work of the chain at the time. 0: a fresh node stores any valid headers.
    arith_uint256 minimum_chain_work;

    // Milestone 10: false while the network's genesis is provisional (mainnet before launch day):
    // its coins will be thrown away, and the node says so.
    bool launched = true;
};

// The networks, as Bitcoin has them: mainnet, where coins are meant to have value; testnet, for
// trying things in public; regtest, a private network for tests on one machine, where any CPU
// mines a block instantly.
enum class Network { Mainnet, Testnet, Regtest };

// Each network's genesis is mined with `boundd genesis`; re-mine it whenever a consensus change
// alters the genesis block.
const ConsensusParams& mainnet_params();
const ConsensusParams& testnet_params();
const ConsensusParams& regtest_params();
const ConsensusParams& params_for(Network network);

// Since launch day (16/Nov/2026), a node without --mainnet, --testnet or --regtest joins mainnet,
// as Bitcoin's does.
inline constexpr Network kDefaultNetwork = Network::Mainnet;

// "mainnet", "testnet", "regtest": the name in the data directory's path and in the RPC.
std::string_view network_name(Network network);
// --mainnet, --testnet or --regtest, or nullopt for anything else.
std::optional<Network> network_flag(std::string_view arg);

// The genesis block: the header plus its coinbase, which genesis.merkle_root must commit to.
inline Block genesis_block(const ConsensusParams& p) { return Block{p.genesis, {p.genesis_coinbase}}; }

}  // namespace bound
