#include "node/node.h"

#include "app/datadir.h"
#include "core/address.h"
#include "core/amount.h"
#include "core/chaindb.h"
#include "core/mempool.h"
#include "core/miner.h"
#include "core/params.h"
#include "core/pow.h"
#include "core/validation.h"
#include "index/chainindex.h"
#include "net/addrman.h"
#include "net/banman.h"
#include "net/connman.h"
#include "net/peerman.h"
#include "rpc/http.h"
#include "rpc/methods.h"
#include "rpc/server.h"
#include "util/serialize.h"
#include "util/time.h"
#include "wallet/wallet.h"

#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace bound {

namespace {

std::uint64_t random_u64() {
    std::random_device rd;
    return std::uint64_t{rd()} << 32 | rd();
}

std::optional<Bytes> read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    return Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Written to a temporary file first, so a crash mid-write can't leave half a file behind.
void write_file(const std::filesystem::path& file, const Bytes& data) {
    const std::filesystem::path temp = file.string() + ".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return;
    }
    std::filesystem::rename(temp, file);
}

AddrMan load_peers(const std::filesystem::path& file, std::uint64_t seed, const NodeLog& log) {
    const std::optional<Bytes> data = read_file(file);
    if (!data) return AddrMan(seed);
    try {
        return AddrMan::deserialize(*data, seed);
    } catch (const DeserializeError&) {
        log("peers.dat is damaged; starting with no known peers");
        return AddrMan(seed);
    }
}

BanMan load_bans(const std::filesystem::path& file, const NodeLog& log) {
    const std::optional<Bytes> data = read_file(file);
    if (!data) return BanMan();
    try {
        return BanMan::deserialize(*data);
    } catch (const DeserializeError&) {
        log("banlist.dat is damaged; starting with no bans");
        return BanMan();
    }
}

// Milestone 7: the RPC cookie, there while the node runs.
class CookieFile {
public:
    CookieFile(std::filesystem::path path, const std::string& cookie) : path_(std::move(path)) {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << cookie;
        if (!out) throw std::runtime_error("can't write " + path_.string());
    }
    ~CookieFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    CookieFile(const CookieFile&) = delete;
    CookieFile& operator=(const CookieFile&) = delete;

private:
    std::filesystem::path path_;
};

std::uint16_t parse_port(std::string_view text) {
    unsigned long port = 0;
    try {
        port = std::stoul(std::string(text));
    } catch (const std::exception&) {
        throw std::invalid_argument("not a port: " + std::string(text));
    }
    if (port == 0 || port > 65535) throw std::invalid_argument("not a port: " + std::string(text));
    return static_cast<std::uint16_t>(port);
}

}  // namespace

std::optional<std::filesystem::path> set_aside_replaced_chain(const std::filesystem::path& datadir, const ConsensusParams& params) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(datadir / "chainstate")) return std::nullopt;
    std::optional<ChainMeta> meta;
    {
        LevelDbStore db(datadir / "chainstate");  // closed again before anything moves
        if (const std::optional<Bytes> stored = db.get(meta_key())) {
            try {
                meta = decode_meta(*stored);
            } catch (const DeserializeError&) {
                return std::nullopt;  // damaged: Chainstate says so
            }
        }
    }
    if (!meta || meta->network != params.network_name || meta->genesis == params.genesis.hash()) return std::nullopt;
    const fs::path to = datadir / ("old-chain-" + meta->genesis.to_hex());
    fs::create_directories(to);
    for (const char* name : {"chainstate", "blocks", "index", "wallet"}) {
        if (fs::exists(datadir / name)) fs::rename(datadir / name, to / name);
    }
    return to;
}

std::optional<NodeOptions> parse_node_options(std::span<const std::string_view> args) {
    NodeOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const bool has_value = i + 1 < args.size();
        if (args[i] == "--nolisten") {
            options.listen = false;
        } else if (args[i] == "--local") {
            options.local = true;
        } else if (args[i] == "--port" && has_value) {
            options.port = parse_port(args[++i]);
        } else if (args[i] == "--rpcport" && has_value) {
            options.rpc_port = parse_port(args[++i]);
        } else if (args[i] == "--connect" && has_value) {
            options.connect.emplace_back(args[++i]);
        } else if (args[i] == "--mine" && has_value) {
            options.mine = std::string(args[++i]);
        } else if (args[i] == "--minethreads" && has_value) {
            const std::string text(args[++i]);
            unsigned long n = 0;
            try {
                n = std::stoul(text);
            } catch (const std::exception&) {
                throw std::invalid_argument("--minethreads: not a number: " + text);
            }
            if (n < 1 || n > 1024) throw std::invalid_argument("--minethreads: from 1 to 1024");
            options.mine_threads = static_cast<unsigned>(n);
        } else if (args[i] == "--seednode" && has_value) {
            options.seednodes.emplace_back(args[++i]);
        } else if (args[i] == "--noseeds") {
            options.use_seeds = false;
        } else if (args[i] == "--allowprivate") {
            options.allow_private = true;
        } else if (args[i] == "--index") {
            options.index = true;
        } else if (args[i] == "--externalip" && has_value) {
            options.external_ips.emplace_back(args[++i]);
        } else if (args[i] == "--maxinbound" && has_value) {
            const std::string text(args[++i]);
            try {
                options.max_inbound = static_cast<std::size_t>(std::stoul(text));
            } catch (const std::exception&) {
                throw std::invalid_argument("--maxinbound: not a number: " + text);
            }
        } else {
            return std::nullopt;
        }
    }
    return options;
}

void run_node(const std::filesystem::path& datadir, const NodeOptions& options, const std::atomic<bool>& interrupt,
              const NodeLog& log) {
    const ConsensusParams& p = params_for(options.network);
    const std::size_t block_budget = p.max_block_size - 1000;  // room after the header and coinbase
    const auto log_line = [&log](const std::string& text) {
        if (log) log(text);
    };

    // Milestone 9: mining switched on and off while the node runs (setmining), --mine to start with.
    MiningControl mining;
    mining.threads = options.mine_threads;
    if (options.mine) {
        mining.payee = decode_address(*options.mine, p.address_version);
        if (!mining.payee) throw std::invalid_argument("not a " + std::string(p.network_name) + " address: " + *options.mine);
    }

    std::filesystem::create_directories(datadir);
    if (const std::optional<std::filesystem::path> old = set_aside_replaced_chain(datadir, p)) {
        log_line(std::format("the {} chain here began at a genesis block this version replaced (the testnet started over "
                             "with BoundHash, or mainnet launched): it and its wallet are kept in {}, and a new chain starts",
                             p.network_name, old->string()));
    }
    DataDir data(datadir);
    Chainstate chain(p, data.db, data.blocks);
    Mempool mempool(chain);
    const std::uint64_t seed = random_u64();
    const std::filesystem::path peers_file = datadir / "peers.dat";
    AddrMan addrman = load_peers(peers_file, seed, log_line);
    // On the public network only public addresses are kept, dialled and passed on. With --local
    // (nodes on one machine) or --allowprivate (on one LAN), any.
    const bool allow_private = options.local || options.allow_private;
    addrman.set_routable_only(!allow_private);
    const std::filesystem::path bans_file = datadir / "banlist.dat";
    BanMan banman = load_bans(bans_file, log_line);

    const std::uint16_t listen_port = options.port != 0 ? options.port : p.default_port;
    PeerConfig peer_config;
    for (const std::string& text : options.external_ips) {
        const std::vector<NetAddress> found = resolve(text, listen_port);
        if (found.empty()) throw std::invalid_argument("--externalip: can't resolve " + text);
        peer_config.external.push_back(found.front());
    }
    PeerManager peers(chain, mempool, addrman, banman, peer_config, seed ^ 0x9e3779b97f4a7c15ULL, log_line);
    ConnmanOptions net;
    net.listen = options.listen;
    net.loopback_only = options.local;
    net.port = listen_port;
    net.default_port = p.default_port;
    net.connect = options.connect;
    if (options.use_seeds) net.seeds = p.seeds;
    net.seeds.insert(net.seeds.end(), options.seednodes.begin(), options.seednodes.end());
    if (options.max_inbound) net.max_inbound = *options.max_inbound;
    Connman connman(peers, addrman, banman, net, log_line, unix_now);
    peers.set_listen_port(connman.port());

    // Milestone 8: the indexes an explorer needs, kept only when asked for.
    std::optional<LevelDbStore> index_db;
    std::optional<ChainIndex> index;
    if (options.index) {
        index_db.emplace(datadir / "index");
        index.emplace(*index_db, chain, log_line);
    }

    // Milestone 7: the wallet (if one was created) and the RPC server.
    LevelDbStore wallet_db(datadir / "wallet");
    NodeContext node{
        .chain = chain,
        .mempool = mempool,
        .peers = peers,
        .clock = unix_now,
        .wallet_db = &wallet_db,
        .wallet = nullptr,
        .wallet_options = {},
        .request_stop = [&connman] { connman.stop(); },
        .p2p_port = connman.port(),
        .user_agent = peer_config.user_agent,
        .banman = &banman,
        .save_bans = [&] { write_file(bans_file, banman.serialize()); },
        .index = index ? &*index : nullptr,
        .mining = &mining,
    };
    if (Wallet::exists(wallet_db)) node.wallet = Wallet::load(wallet_db, chain, mempool);
    RpcTable rpc;
    register_node_methods(rpc, node);
    const std::string cookie = make_cookie();
    HttpServer http(options.rpc_port != 0 ? options.rpc_port : p.default_rpc_port,
                    [&](const HttpRequest& request) { return serve_json_rpc(rpc, cookie, request); });
    const CookieFile cookie_file(cookie_path(datadir), cookie);
    connman.add_client(http);

    log_line(std::format("BOUND {} node: height {}, {} known peer addresses, data in {}", p.network_name, chain.height(),
                         addrman.size(), datadir.string()));
    if (!p.launched) {
        log_line(std::format("WARNING: {} hasn't launched. Its genesis is provisional, and every coin on this chain will be "
                             "thrown away when the real one is mined.",
                             p.network_name));
    }
    log_line(options.listen ? std::format("listening on port {}", connman.port()) : "not listening");
    for (const NetAddress& addr : peer_config.external) log_line("external address " + addr.to_string());
    log_line(std::format("JSON-RPC on 127.0.0.1:{}", http.port()));
    log_line(net.seeds.empty() ? "no seeds: peers come from --connect and from the addresses already known"
                               : std::format("{} seeds to ask for peers if needed", net.seeds.size()));
    if (index) log_line("keeping the transaction and address indexes");
    log_line(node.wallet ? std::format("wallet: {} transactions, {} {} spendable", node.wallet->transaction_count(),
                                       format_coins(node.wallet->balance().spendable), kTicker)
                         : "no wallet (bound-cli createwallet <passphrase> makes one)");

    std::optional<BackgroundMiner> miner;
    std::optional<PublicKey> mining_to;   // the payee `miner` was set up for
    unsigned mining_threads = 0;          // and its thread count
    const BlockIndex* work_tip = nullptr;  // the tip the miner's current work builds on
    std::int64_t work_time = 0;
    std::size_t work_mempool = 0;  // the mempool's size when that work was made
    const std::int64_t started = unix_now();
    std::int64_t saved = started;
    std::int64_t expired = started;
    std::int64_t resubmitted = started - 600;  // our own lost transactions go out again soon after starting
    std::int64_t last_rate = started;          // for the log, once a minute
    std::uint64_t last_hashes = 0;
    std::int64_t sample_time = started;  // for getmininginfo, every few seconds
    std::uint64_t sample_hashes = 0;

    connman.run([&] {
        if (interrupt) connman.stop();
        const std::int64_t now = unix_now();
        const unsigned threads = mining.threads != 0 ? mining.threads : BackgroundMiner::default_threads();
        if (mining.payee != mining_to || (mining_to && threads != mining_threads)) {  // setmining, or --mine at the start
            miner.reset();
            mining_to = mining.payee;
            mining_threads = threads;
            if (mining_to) {
                miner.emplace(p, threads);
                log_line(std::format("mining to {} on {} thread{}", encode_address(*mining_to, p.address_version), threads,
                                     threads == 1 ? "" : "s"));
            } else {
                log_line("stopped mining");
            }
            work_tip = nullptr;
            last_rate = sample_time = now;
            last_hashes = sample_hashes = 0;
            mining.hashes_per_second = 0;
            mining.paused = false;
        }
        if (miner) {
            if (const std::optional<Block> solved = miner->take_solved()) {
                const BlockAcceptResult r = peers.submit_block(*solved, now);
                const BlockIndex* found = chain.find(solved->hash());
                if (r.accepted() && found != nullptr) {
                    ++mining.blocks_found;
                    log_line(std::format("mined #{} {} with {} transactions{}", found->height, found->hash.to_hex(), solved->txs.size() - 1,
                                         &chain.tip() == found ? "" : " (not the tip: someone else's block won)"));
                } else {
                    log_line("our own block was rejected");
                }
            }
            // Fresh work when the tip moves, when new transactions arrive (at most every 5 s),
            // and every 30 s (for a current timestamp). No mining during the initial sync: those
            // blocks would only go to waste. (Only then: headers without blocks mustn't be able
            // to stop the miner; see PeerManager::initial_sync.)
            const bool syncing = peers.initial_sync(now);
            const bool stale = &chain.tip() != work_tip || now - work_time >= 30 ||
                               (mempool.size() != work_mempool && now - work_time >= 5);
            mining.paused = syncing;
            if (syncing) {
                if (work_tip != nullptr) miner->set_work(std::nullopt);
                work_tip = nullptr;
            } else if (stale) {
                const auto time = static_cast<std::uint32_t>(now);
                std::optional<Block> tmpl = make_block_template(chain, *mining_to, mempool.select(block_budget).txs, time);
                if (!tmpl) {
                    log_line("the mempool's transactions don't make a valid block; mining an empty one");
                    tmpl = make_block_template(chain, *mining_to, {}, time);
                }
                miner->set_work(std::move(tmpl));
                work_tip = &chain.tip();
                work_time = now;
                work_mempool = mempool.size();
            }
            const std::uint64_t hashes = miner->hashes();
            if (now - sample_time >= 5) {
                mining.hashes_per_second = static_cast<double>(hashes - sample_hashes) / static_cast<double>(now - sample_time);
                sample_time = now;
                sample_hashes = hashes;
            }
            if (now - last_rate >= 60) {
                log_line(std::format("mining at {:.0f} H/s, difficulty {:.2f}",
                                     static_cast<double>(hashes - last_hashes) / static_cast<double>(now - last_rate),
                                     difficulty(next_work_required(chain.tip(), p), p)));
                last_rate = now;
                last_hashes = hashes;
            }
        }
        if (node.wallet) {
            node.wallet->tick(now);
            if (now - resubmitted >= 600) {
                for (const Transaction& tx : node.wallet->to_resubmit()) peers.submit_transaction(tx, now);
                resubmitted = now;
            }
        }
        if (now - expired >= 60) {
            if (const std::size_t n = mempool.expire(now); n > 0) log_line(std::format("{} transactions expired from the mempool", n));
            expired = now;
        }
        if (now - saved >= 300) {
            write_file(peers_file, addrman.serialize());
            saved = now;
        }
    });
    miner.reset();
    write_file(peers_file, addrman.serialize());
    log_line(std::format("stopped at height {}", chain.height()));
}

}  // namespace bound
