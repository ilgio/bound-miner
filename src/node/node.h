#pragma once

// The node itself: what `boundd node` runs, and what the desktop wallet (bound-qt) runs inside
// itself, as Bitcoin-Qt does.

#include "core/leveldb_store.h"
#include "core/params.h"
#include "core/storage.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bound {

// What a data directory holds: the database (block tree, UTXO set, undo data) and the block files.
struct DataDir {
    explicit DataDir(const std::filesystem::path& dir) : db(dir / "chainstate"), blocks(dir / "blocks") {}

    LevelDbStore db;
    FlatFileBlockStore blocks;
};

// Milestone 16: when this network's genesis has changed since the data directory was used (the
// testnet starting over with BoundHash; mainnet's launch replacing its provisional genesis), the
// chain stored there can never continue. Moves its block data, indexes and wallet into
// <datadir>/old-chain-<old genesis id>/ so a new chain can start, and returns that folder. Does
// nothing (nullopt) for a directory without a chain, with this genesis's chain, or with another
// network's chain (which Chainstate then refuses).
std::optional<std::filesystem::path> set_aside_replaced_chain(const std::filesystem::path& datadir, const ConsensusParams& params);

struct NodeOptions {
    Network network = kDefaultNetwork;  // milestone 10; set from --mainnet, --testnet, --regtest
    std::uint16_t port = 0;      // P2P; 0: the network's default
    std::uint16_t rpc_port = 0;  // JSON-RPC on 127.0.0.1; 0: the network's default
    bool listen = true;
    bool local = false;  // listen on 127.0.0.1 only: for trying several nodes on one machine
    std::vector<std::string> connect;
    std::optional<std::string> mine;  // pay mined blocks to this address
    unsigned mine_threads = 0;        // milestone 12: --minethreads; 0: every core but one
    // Milestone 8.
    std::vector<std::string> seednodes;      // asked for addresses, besides the network's own seeds
    bool use_seeds = true;                   // the network's built-in seeds
    bool allow_private = false;              // take and pass on private addresses (a LAN)
    bool index = false;                      // keep the transaction and address indexes
    std::optional<std::size_t> max_inbound;  // inbound connections to take
    std::vector<std::string> external_ips;   // where others reach us (advertised to peers)
};

// The node options in `args` (--port, --connect, ...; see boundd's usage). nullopt for an option it
// doesn't know or one missing its value; throws std::invalid_argument for a bad value. The
// network isn't among them: the programs take it first (take_network in app/datadir.h).
std::optional<NodeOptions> parse_node_options(std::span<const std::string_view> args);

// One line for the node's log, without a timestamp: the caller adds one if it wants.
using NodeLog = std::function<void(const std::string&)>;

// Runs a node on `datadir` until it's told to stop: by the `stop` RPC, or by `interrupt` turning
// true (checked several times a second). Throws if it can't start, e.g. when another node is
// using the data directory or the ports are taken.
void run_node(const std::filesystem::path& datadir, const NodeOptions& options, const std::atomic<bool>& interrupt,
              const NodeLog& log);

}  // namespace bound
