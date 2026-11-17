// boundd, the BOUND node. The network comes first: --mainnet, --testnet or --regtest, and
// kDefaultNetwork (core/params.h) if none. Each keeps its chain in its own data directory (default
// %APPDATA%\BOUND\<network>, or ~/.bound/<network>):
//   boundd [network] [--datadir <dir>] node [--port <p>] [--rpcport <p>] [--nolisten | --local]
//                                 [--connect <host[:port]>]... [--seednode <host[:port]>]...
//                                 [--noseeds] [--maxinbound <n>] [--allowprivate] [--index]
//                                 [--externalip <ip[:port]>]... [--mine <address>] [--minethreads <n>]
//                                                 run a node on the network until Ctrl+C or
//                                                 `bound-cli stop`. JSON-RPC on 127.0.0.1
//                                                 (bound-cli talks to it).
//       --connect      a peer to stay connected to
//       --seednode     a name to ask for peers' addresses, besides the built-in seeds (--noseeds
//                      leaves those out)
//       --local        listen on 127.0.0.1 only, and accept local addresses: several nodes on one
//                      machine
//       --allowprivate take and pass on private addresses (10.x, 192.168.x, ...): nodes on one LAN
//       --index        keep the transaction and address indexes an explorer needs
//       --externalip   the address others reach this node at (the port forwarded to it); without
//                      it the node advertises where its outbound peers see it
//   boundd [--datadir <dir>] mine <n> [address]   mine n blocks on the stored chain, offline
//   boundd [--datadir <dir>] info                 height, tip, difficulty, work, supply
//   boundd [--datadir <dir>] balance <address>
// and two commands run entirely in memory:
//   boundd demo                     mine, wait out coinbase maturity, pay someone through the
//                                   mempool, lose the payment to a reorganisation, and see the
//                                   mempool put it back
//   boundd genesis [message] [time | now]
//                                   mine a genesis block for the network on every core, and print
//                                   the values for params.cpp; without arguments, the network's
//                                   own genesis again, which checks it (milestone 15)
#include "app/datadir.h"
#include "core/address.h"
#include "core/consensus.h"
#include "core/genesis.h"
#include "core/leveldb_store.h"
#include "core/mempool.h"
#include "core/miner.h"
#include "core/params.h"
#include "core/pow.h"
#include "core/storage.h"
#include "core/validation.h"
#include "core/wallet.h"
#include "node/node.h"
#include "util/serialize.h"
#include "util/time.h"
#include "util/version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bound;

// Milestone 10: the network the command works on (--mainnet, --testnet, --regtest).
Network g_network = kDefaultNetwork;
const ConsensusParams& params() { return params_for(g_network); }

PublicKey parse_address(std::string_view address) {
    const std::optional<PublicKey> key = decode_address(address, params().address_version);
    if (!key) throw std::invalid_argument("not a " + std::string(params().network_name) + " address: " + std::string(address));
    return *key;
}

std::string format_amount(Amount a) { return format_coins(a) + " " + std::string(kTicker); }

std::string address_of(const KeyPair& key) { return encode_address(key.public_key(), params().address_version); }

// Room a block leaves for transactions after its header and coinbase.
std::size_t block_budget(const ConsensusParams& p) { return p.max_block_size - 1000; }

// Mines the next block on `chain`, paying `miner` and including `txs`, accepts it, and prints a line.
void mine_next(Chainstate& chain, const PublicKey& miner, std::vector<Transaction> txs, bool print = true) {
    const auto start = std::chrono::steady_clock::now();
    std::optional<Block> tmpl = make_block_template(chain, miner, std::move(txs), static_cast<std::uint32_t>(unix_now()));
    if (!tmpl) throw std::runtime_error("a transaction for the block template is invalid");
    const Block block = mine_block(std::move(*tmpl), chain.params());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!chain.accept_block(block, unix_now()).accepted()) throw std::runtime_error("our own block was rejected");
    if (print) {
        const double hs = seconds > 0 ? (block.header.nonce + 1.0) / seconds : 0;
        std::cout << std::format("  #{:<4} {}  nonce {:>10}  {:5.2f} s  {:7.0f} H/s  difficulty {:.2f}  {} tx\n",
                                 chain.height(), block.hash().to_hex(), block.header.nonce, seconds, hs,
                                 difficulty(block.header.bits, chain.params()), block.txs.size());
    }
}

// Accepts a block from another node; anything but a clean accept is a bug in the demo.
void receive(Chainstate& chain, const Block& block) {
    if (!chain.accept_block(block, unix_now()).accepted()) throw std::runtime_error("a node rejected a valid block");
}

int cmd_demo() {
    const ConsensusParams& p = params();
    Chainstate chain(p);  // Alice's node
    Mempool mempool(chain);
    const KeyPair alice = KeyPair::generate();
    const KeyPair bob = KeyPair::generate();
    const KeyPair carol = KeyPair::generate();

    std::cout << "BOUND " << p.network_name << ", in memory. Genesis " << p.genesis_hash.to_hex() << "\n"
              << "Alice " << address_of(alice) << "\nBob   " << address_of(bob) << "\nCarol " << address_of(carol)
              << "\n\n1. Alice mines 101 blocks (coinbase outputs need " << p.coinbase_maturity << " blocks to mature):\n";
    for (int i = 1; i <= 101; ++i) mine_next(chain, alice.public_key(), {}, i <= 3 || i % 25 == 0 || i == 101);

    // Carol runs a node of her own. It has every block so far, and is about to lose touch with Alice's.
    Chainstate carols_node(p);
    for (int h = 1; h <= chain.height(); ++h) receive(carols_node, chain.block_at(h));

    const int next = chain.height() + 1;
    Amount spendable = 0;
    for (const auto& [op, coin] : spendable_coins(chain.utxos(), alice.public_key(), next, p)) spendable += coin.out.amount;
    std::cout << "\n2. Alice owns " << format_amount(balance(chain.utxos(), alice.public_key())) << ", but only "
              << format_amount(spendable) << " is mature enough to spend in block #" << next << ".\n";

    const Amount amount = 7 * kCoin + kCoin / 2;
    const Amount fee = kCoin / 1000;
    std::optional<Transaction> pay = create_payment(chain.utxos(), alice, bob.public_key(), amount, fee, next, p);
    if (!pay) throw std::runtime_error("Alice can't afford the payment");
    const MempoolAccept accepted = mempool.accept(*pay, unix_now());
    if (!accepted.accepted()) throw std::runtime_error("the mempool refused the payment: " + accepted.reason);
    const MempoolEntry& entry = *mempool.find(pay->txid());
    std::cout << "\n3. Alice pays Bob " << format_amount(amount) << " (fee " << format_amount(fee) << "), txid "
              << pay->txid().to_hex() << ".\n   It waits in her node's mempool: " << entry.size << " bytes, "
              << FeeRate::of(entry.fee, entry.size).per_kb / 1000 << " units per byte. The next block takes it:\n";
    mine_next(chain, alice.public_key(), mempool.select(block_budget(p)).txs);

    std::cout << "\n4. After block #" << chain.height() << " (mempool: " << mempool.size() << " waiting):\n"
              << "   Alice  " << format_amount(balance(chain.utxos(), alice.public_key())) << "\n"
              << "   Bob    " << format_amount(balance(chain.utxos(), bob.public_key())) << "\n"
              << "   supply " << format_amount(chain.utxos().total_value()) << " in " << chain.utxos().size()
              << " unspent outputs (" << chain.height() << " blocks x " << format_amount(p.initial_subsidy)
              << "; fees only move coins)\n";

    const BlockIndex& alices_tip = chain.tip();

    std::cout << "\n5. Meanwhile Carol, cut off from Alice, mines two blocks on #101 on her own node:\n";
    mine_next(carols_node, carol.public_key(), {});
    mine_next(carols_node, carol.public_key(), {});

    std::cout << "\n6. The connection comes back, and Alice's node receives Carol's blocks:\n";
    receive(chain, carols_node.block_at(102));
    if (&chain.tip() != &alices_tip) throw std::runtime_error("an equal-work branch replaced the tip");
    std::cout << "   Carol's #102: the same work as Alice's #102. Stored on a side branch; the tip stays Alice's,\n"
              << "   because of two chains with equal work the first one seen wins.\n";
    receive(chain, carols_node.block_at(103));
    if (chain.tip().hash != carols_node.tip().hash) throw std::runtime_error("the branch with more work didn't take over");
    std::cout << std::format("   Carol's #103: her branch now has more work ({:.0f} expected hashes against {:.0f}).\n",
                             chain.tip().chain_work.to_double(), alices_tip.chain_work.to_double())
              << "   Reorg: Alice's #102 is disconnected using its undo data, Carol's #102 and #103 are connected.\n";

    std::cout << "\n7. After the reorg, tip #" << chain.height() << " " << chain.tip().hash.to_hex() << ":\n"
              << "   Bob    " << format_amount(balance(chain.utxos(), bob.public_key())) << "\n"
              << "   Bob's payment was only in the block that lost. Alice's mempool took it back: "
              << (mempool.contains(pay->txid()) ? "it's waiting again" : "it's MISSING") << " (" << mempool.size()
              << " waiting).\n";
    if (!mempool.contains(pay->txid())) throw std::runtime_error("the mempool lost the payment");

    std::cout << "\n8. Alice mines the next block on the new chain, and the payment confirms again:\n";
    mine_next(chain, alice.public_key(), mempool.select(block_budget(p)).txs);
    std::cout << "   Alice  " << format_amount(balance(chain.utxos(), alice.public_key())) << "\n"
              << "   Bob    " << format_amount(balance(chain.utxos(), bob.public_key())) << "\n"
              << "   Carol  " << format_amount(balance(chain.utxos(), carol.public_key())) << "\n"
              << "   supply " << format_amount(chain.utxos().total_value()) << " (" << chain.height() << " blocks x "
              << format_amount(p.initial_subsidy) << ")\n";
    return 0;
}

int cmd_mine(const std::filesystem::path& datadir, int count, std::optional<std::string_view> address) {
    const ConsensusParams& p = params();
    DataDir data(datadir);
    Chainstate chain(p, data.db, data.blocks);
    PublicKey miner{};
    if (address) {
        miner = parse_address(*address);
    } else {
        miner = KeyPair::generate().public_key();
        std::cout << "No address given: paying a throwaway key, so nobody can spend these coins.\n";
    }
    std::cout << "Mining " << count << " blocks on #" << chain.height() << " in " << datadir.string() << " to "
              << encode_address(miner, p.address_version) << "\n";
    for (int i = 0; i < count; ++i) mine_next(chain, miner, {});
    std::cout << "Tip #" << chain.height() << ". That address holds " << format_amount(balance(chain.utxos(), miner))
              << "\n";
    return 0;
}

// Milestone 6: the node. It lives in src/node/node.cpp, shared with the desktop wallet.

std::atomic<bool> g_interrupted{false};  // set by Ctrl+C, or SIGTERM from a service manager

void on_interrupt(int) { g_interrupted = true; }

void log_line(const std::string& text) { std::cout << format_utc(unix_now()) << " " << text << "\n" << std::flush; }

int cmd_node(const std::filesystem::path& datadir, const NodeOptions& options) {
    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);
    run_node(datadir, options, g_interrupted, log_line);
    return 0;
}

int cmd_info(const std::filesystem::path& datadir) {
    const ConsensusParams& p = params();
    DataDir data(datadir);
    const Chainstate chain(p, data.db, data.blocks);
    const BlockIndex& tip = chain.tip();
    const int next_retarget = (tip.height / p.retarget_interval + 1) * p.retarget_interval;
    const std::string retarget = p.pow_no_retargeting  ? std::string("fixed")
                                 : p.asert_half_life > 0 ? std::string("adjusted every block (ASERT)")
                                                         : std::format("retarget at #{}", next_retarget);
    std::cout << std::format("data directory   {}\n", datadir.string())
              << std::format("network          {}{}\n", p.network_name, p.launched ? "" : " (not launched: a provisional genesis)")
              << std::format("height           {}\n", tip.height)
              << std::format("tip              {}  {} UTC\n", tip.hash.to_hex(), format_utc(tip.header.time))
              << std::format("difficulty       {:.2f} for the next block, {}\n", difficulty(next_work_required(tip, p), p), retarget)
              << std::format("chain work       {:.0f} expected hashes\n", tip.chain_work.to_double())
              << std::format("blocks known     {} ({} off the active chain)\n", chain.tree_size(),
                             chain.tree_size() - static_cast<std::size_t>(tip.height + 1))
              << std::format("unspent outputs  {}, worth {}\n", chain.utxos().size(),
                             format_amount(chain.utxos().total_value()));
    return 0;
}

int cmd_balance(const std::filesystem::path& datadir, std::string_view address) {
    const ConsensusParams& p = params();
    const PublicKey owner = parse_address(address);
    DataDir data(datadir);
    const Chainstate chain(p, data.db, data.blocks);
    Amount spendable = 0;
    for (const auto& [op, coin] : spendable_coins(chain.utxos(), owner, chain.height() + 1, p)) spendable += coin.out.amount;
    std::cout << format_amount(balance(chain.utxos(), owner)) << " at height " << chain.height() << ", of which "
              << format_amount(spendable) << " is spendable in the next block\n";
    return 0;
}

// For a C++ string literal: in printable ASCII only backslashes and quotes need escaping.
std::string cpp_string(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

// Milestone 15: mines the network's genesis on every core (core/genesis.h) and prints the lines for
// params.cpp. Launch day runs it with that day's headline. With no message and time it mines the
// network's own genesis again, which checks it: the search always finds the same block.
int cmd_genesis(std::string_view message, std::uint32_t time) {
    const ConsensusParams& p = params();
    for (const char c : message) {
        if (c < 0x20 || c > 0x7e) throw std::invalid_argument("the message must be printable ASCII");
    }
    const std::optional<Block> unmined = make_genesis(message, block_subsidy(0, p), time, p.pow_limit_bits);
    if (!unmined) throw std::invalid_argument(std::format("the message is {} characters; at most {} fit", message.size(), Transaction::kMaxMemo));

    std::signal(SIGINT, on_interrupt);
    std::signal(SIGTERM, on_interrupt);
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::cerr << std::format("Mining {}'s genesis on {} threads, from {} UTC (Ctrl+C stops)\n", p.network_name, threads, format_utc(time));
    const auto started = std::chrono::steady_clock::now();
    const auto seconds = [&] {
        return std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count());
    };
    std::stop_source stop;
    std::int64_t reported = 0;
    const std::optional<BlockHeader> header = mine_genesis(unmined->header, p, threads, stop.get_token(), [&](std::uint64_t hashes) {
        if (g_interrupted) stop.request_stop();
        const std::int64_t s = seconds();
        if (s - reported < 10) return;
        reported = s;
        std::cerr << std::format("  {:.2f} million hashes in {} s, {:.0f} H/s\n", static_cast<double>(hashes) / 1e6, s,
                                 static_cast<double>(hashes) / static_cast<double>(s));
    });
    if (!header) throw std::runtime_error("stopped before finding it");

    const uint256 hash = header->hash();
    std::cerr << std::format("Found in {} s\n", seconds());
    std::cout << std::format("{} genesis\n", p.network_name)
              << std::format("  hash         {}  (its id: SHA-256d)\n", hash.to_hex())
              << std::format("  BoundHash    {}  (its proof of work)\n", pow_hash(*header, p).to_hex())
              << std::format("  merkle root  {}\n", header->merkle_root.to_hex())
              << std::format("  time         {} ({} UTC)\n", header->time, format_utc(header->time))
              << std::format("  bits         0x{:08x}\n", header->bits)
              << std::format("  nonce        {}\n", header->nonce)
              << std::format("  message      {}\n\n", message)
              << "For src/core/params.cpp:\n"
              << std::format("    p.genesis_coinbase = make_coinbase(0, {} * kCoin, PublicKey{{}},\n", block_subsidy(0, p) / kCoin)
              << std::format("                                       bytes_of(\"{}\"));\n", cpp_string(message))
              << "    p.genesis.version = 1;\n"
              << "    p.genesis.merkle_root = genesis_block(p).compute_merkle_root();\n"
              << std::format("    p.genesis.time = {};  // {} UTC\n", header->time, format_utc(header->time))
              << "    p.genesis.bits = p.pow_limit_bits;\n"
              << std::format("    p.genesis.nonce = {};\n", header->nonce)
              << std::format("    p.genesis_hash = uint256::from_hex(\"{}\").value();\n", hash.to_hex());
    return 0;
}

int usage() {
    std::cerr << "usage: boundd [network] [--datadir <dir>] node [--port <port>] [--rpcport <port>] [--nolisten | --local]\n"
                 "                                    [--connect <host[:port]>]... [--seednode <host[:port]>]...\n"
                 "                                    [--noseeds] [--maxinbound <n>] [--allowprivate] [--index]\n"
                 "                                    [--externalip <ip[:port]>]... [--mine <address>] [--minethreads <n>]\n"
                 "       boundd [network] [--datadir <dir>] mine <count> [address]\n"
                 "       boundd [network] [--datadir <dir>] info\n"
                 "       boundd [network] [--datadir <dir>] balance <address>\n"
                 "       boundd [network] demo\n"
                 "       boundd [network] genesis [message] [unix-time | now]\n"
                 "       boundd --version\n"
                 "The network is --mainnet, --testnet or --regtest; --"
              << network_name(kDefaultNetwork) << " if none is given.\nThe data directory defaults to "
              << default_datadir(g_network).string() << ". Talk to a running node with bound-cli.\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (bound::asks_for_version(argc, argv)) {
        std::cout << bound::version_text("boundd");
        return 0;
    }
    try {
        std::vector<std::string_view> args(argv + 1, argv + argc);
        // The network and --datadir come first, in either order.
        g_network = take_network(args, 3);
        std::optional<std::filesystem::path> chosen_datadir;
        if (args.size() >= 2 && args[0] == "--datadir") {
            chosen_datadir = std::filesystem::path(args[1]);
            args.erase(args.begin(), args.begin() + 2);
        }
        const std::filesystem::path datadir = chosen_datadir.value_or(default_datadir(g_network));
        if (args.empty()) return usage();
        if (args[0] == "node") {
            std::optional<NodeOptions> options = parse_node_options(std::span(args).subspan(1));
            if (!options) return usage();
            options->network = g_network;
            return cmd_node(datadir, *options);
        }
        if (args[0] == "mine" && (args.size() == 2 || args.size() == 3)) {
            const std::optional<std::string_view> address = args.size() == 3 ? std::optional(args[2]) : std::nullopt;
            return cmd_mine(datadir, std::stoi(std::string(args[1])), address);
        }
        if (args[0] == "info" && args.size() == 1) return cmd_info(datadir);
        if (args[0] == "balance" && args.size() == 2) return cmd_balance(datadir, args[1]);
        if (args[0] == "demo" && args.size() == 1) return cmd_demo();
        if (args[0] == "genesis" && args.size() <= 3) {
            const std::string default_message = std::string(
                params().genesis_coinbase.memo.begin(), params().genesis_coinbase.memo.end());
            const std::string_view message = args.size() >= 2 ? args[1] : std::string_view(default_message);
            const auto time = args.size() < 3      ? params().genesis.time
                              : args[2] == "now" ? static_cast<std::uint32_t>(unix_now())
                                                 : static_cast<std::uint32_t>(std::stoul(std::string(args[2])));
            return cmd_genesis(message, time);
        }
        return usage();
    } catch (const std::exception& e) {
        std::cerr << "boundd: " << e.what() << "\n";
        return 1;
    }
}
