#include "rpc/methods.h"

#include "core/address.h"
#include "core/consensus.h"
#include "core/miner.h"
#include "core/pow.h"
#include "net/protocol.h"
#include "util/hex.h"
#include "util/serialize.h"
#include "wallet/mnemonic.h"

#include <sodium.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <utility>
#include <vector>

namespace bound {

namespace {

std::string address_of(const PublicKey& key, const ConsensusParams& params) { return encode_address(key, params.address_version); }

PublicKey parse_address(const std::string& text, const ConsensusParams& params) {
    const std::optional<PublicKey> key = decode_address(text, params.address_version);
    if (!key) throw RpcError(rpc_error::kInvalidAddressOrKey, "not a " + std::string(params.network_name) + " address: " + text);
    return *key;
}

uint256 parse_hash(const std::string& text, const char* what) {
    const std::optional<uint256> hash = uint256::from_hex(text);
    if (!hash) throw RpcError(rpc_error::kInvalidParameter, std::string(what) + " must be 64 hex digits");
    return *hash;
}

Transaction parse_transaction(const std::string& hex) {
    const std::optional<Bytes> bytes = from_hex(hex);
    if (!bytes) throw RpcError(rpc_error::kDeserialization, "the transaction isn't hex");
    try {
        return Transaction::deserialize(*bytes);
    } catch (const DeserializeError& e) {
        throw RpcError(rpc_error::kDeserialization, std::string("can't decode the transaction: ") + e.what());
    }
}

// Fee rates go in and out as base units per byte, like Bitcoin's sat/vB; decimals allowed.
double units_per_byte(FeeRate rate) { return static_cast<double>(rate.per_kb) / 1000.0; }

FeeRate parse_fee_rate(const RpcParams& p, std::size_t i) {
    const double v = p.number(i);
    if (!(v >= 0) || v > 1e8) throw RpcError(rpc_error::kInvalidParameter, "fee_rate must be from 0 to 100000000 units per byte");
    return FeeRate{static_cast<Amount>(std::llround(v * 1000))};
}

Wallet& need_wallet(NodeContext& node) {
    if (!node.wallet) throw RpcError(rpc_error::kNoWallet, "no wallet: create one with createwallet, or restorewallet");
    return *node.wallet;
}

// Milestone 17: the seed as people write it down (24 words) and as before (hex).
json seed_json(const Seed& seed) {
    std::string words = seed_to_words(seed);
    std::string hex = to_hex(seed);
    json result = {{"words", words}, {"seed", hex}};
    sodium_memzero(words.data(), words.size());
    sodium_memzero(hex.data(), hex.size());
    return result;
}

int code_for(WalletError::Kind kind) {
    switch (kind) {
    case WalletError::Kind::Locked: return rpc_error::kUnlockNeeded;
    case WalletError::Kind::WrongPassphrase: return rpc_error::kWrongPassphrase;
    case WalletError::Kind::InsufficientFunds: return rpc_error::kInsufficientFunds;
    case WalletError::Kind::KeypoolEmpty: return rpc_error::kKeypoolRanOut;
    case WalletError::Kind::NotFound: return rpc_error::kInvalidAddressOrKey;
    case WalletError::Kind::Invalid: return rpc_error::kWallet;
    }
    return rpc_error::kWallet;
}

// A wallet method's WalletError becomes the matching RPC error.
RpcTable::Method wallet_method(RpcTable::Method f) {
    return [f = std::move(f)](const RpcParams& p) -> json {
        try {
            return f(p);
        } catch (const WalletError& e) {
            throw RpcError(code_for(e.kind), e.what());
        }
    };
}

bool printable(const Bytes& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t c) { return c >= 0x20 && c < 0x7f; });
}

json header_to_json(const BlockIndex& node, const Chainstate& chain) {
    const bool active = chain.in_active_chain(node);
    json j = {
        {"hash", node.hash.to_hex()},
        {"confirmations", active ? chain.height() - node.height + 1 : -1},
        {"height", node.height},
        {"version", node.header.version},
        {"merkleroot", node.header.merkle_root.to_hex()},
        {"time", node.header.time},
        {"mediantime", node.median_time_past(chain.params().median_time_span)},
        {"nonce", node.header.nonce},
        {"bits", std::format("{:08x}", node.header.bits)},
        {"difficulty", difficulty(node.header.bits, chain.params())},
        {"chainwork", node.chain_work.to_uint256().to_hex()},
    };
    if (node.parent != nullptr) j["previousblockhash"] = node.parent->hash.to_hex();
    if (active && node.height < chain.height()) j["nextblockhash"] = chain.at(node.height + 1).hash.to_hex();
    return j;
}

const BlockIndex& find_block(const Chainstate& chain, const std::string& hex) {
    const BlockIndex* node = chain.find(parse_hash(hex, "blockhash"));
    if (node == nullptr) throw RpcError(rpc_error::kInvalidAddressOrKey, "no such block");
    return *node;
}

json entry_to_json(const MempoolEntry& e) {
    json depends = json::array();
    for (const uint256& p : e.parents) depends.push_back(p.to_hex());
    json spent_by = json::array();
    for (const uint256& c : e.children) spent_by.push_back(c.to_hex());
    return {
        {"wtxid", e.wtxid.to_hex()},
        {"size", e.size},
        {"fee", format_coins(e.fee)},
        {"fee_rate", units_per_byte(FeeRate::of(e.fee, e.size))},
        {"time", e.time},
        {"depends", depends},
        {"spentby", spent_by},
    };
}

// A wallet transaction the way listtransactions shows it. `amount` is what left or reached the
// wallet, the fee apart: negative for a payment to someone else.
json summary_to_json(const WalletTxSummary& s, const Wallet& wallet, const ConsensusParams& params) {
    std::string category;
    if (s.coinbase) {
        category = s.confirmations < 0 ? "orphan" : s.confirmations < params.coinbase_maturity ? "immature" : "generate";
    } else {
        category = s.from_me ? "send" : "receive";
    }
    json j = {
        {"txid", s.txid.to_hex()},
        {"category", category},
        {"amount", format_coins(s.from_me ? s.received - s.sent + s.fee : s.received)},
        {"confirmations", s.confirmations},
        {"time", s.time},
    };
    if (s.from_me) j["fee"] = format_coins(s.fee);
    if (const WalletTx* w = wallet.find(s.txid); w != nullptr) {
        if (s.confirmations > 0) {
            j["blockhash"] = w->block.to_hex();
            j["blockheight"] = w->height;
        }
        // Milestone 9, for the wallet app: who it went to. For a payment we made, the first output
        // that isn't ours (our own address if all are); for one we received, the first that is.
        const TxOut* shown = nullptr;
        for (const TxOut& out : w->tx.outputs) {
            if (wallet.is_mine(out.recipient) != s.from_me) {
                shown = &out;
                break;
            }
        }
        if (shown == nullptr && !w->tx.outputs.empty()) shown = &w->tx.outputs.front();
        if (shown != nullptr) j["address"] = address_of(shown->recipient, params);
    }
    return j;
}

// Milestone 8.

ChainIndex& need_index(NodeContext& node) {
    if (node.index == nullptr) throw RpcError(rpc_error::kMisc, "this node keeps no index: start it with --index");
    return *node.index;
}

// The coins the transaction at `position` in a block spent, from the block's undo data: one per
// input, in order. Empty if the undo data isn't there.
std::vector<Coin> spent_coins(const Chainstate& chain, const BlockIndex& node, const Block& block, std::size_t position) {
    const std::optional<BlockUndo> undo = chain.read_undo(node);
    if (!undo || position >= block.txs.size()) return {};
    std::size_t first = 0;
    for (std::size_t i = 0; i < position; ++i) first += block.txs[i].inputs.size();
    const std::size_t count = block.txs[position].inputs.size();
    if (first + count > undo->spent.size()) return {};
    const auto begin = undo->spent.begin() + static_cast<std::ptrdiff_t>(first);
    return {begin, begin + static_cast<std::ptrdiff_t>(count)};
}

// A transaction, and where the node found it.
struct LocatedTx {
    Transaction tx;
    const MempoolEntry* entry = nullptr;  // waiting in the mempool...
    const BlockIndex* block = nullptr;    // ...or in this block of the active chain; neither: only the wallet has it
    std::size_t position = 0;             // its place in that block
};

// The mempool first, then the index (any confirmed transaction), then the wallet's own records.
std::optional<LocatedTx> locate_transaction(NodeContext& node, const uint256& txid) {
    if (const MempoolEntry* e = node.mempool.find(txid)) return LocatedTx{e->tx, e, nullptr, 0};
    if (node.index != nullptr) {
        if (std::optional<ChainIndex::FoundTx> found = node.index->read_tx(txid)) {
            return LocatedTx{std::move(found->tx), nullptr, found->block, found->position};
        }
    }
    if (!node.wallet) return std::nullopt;
    const WalletTx* w = node.wallet->find(txid);
    if (w == nullptr) return std::nullopt;
    LocatedTx located{w->tx, nullptr, nullptr, 0};
    if (node.wallet->status(*w) == TxStatus::Confirmed) {
        located.block = node.chain.find(w->block);
        const Block block = node.chain.read_block(*located.block);
        for (std::size_t i = 0; i < block.txs.size(); ++i) {
            if (block.txs[i].txid() == txid) located.position = i;
        }
    }
    return located;
}

// A transaction with what an explorer shows: where it is and, from verbosity 2, what it spent,
// its fee, and which of its outputs have been spent.
json detailed_transaction(NodeContext& node, const LocatedTx& t, int verbosity) {
    const ConsensusParams& params = node.chain.params();
    const uint256 txid = t.tx.txid();
    json j = transaction_to_json(t.tx, params);
    j["hex"] = to_hex(t.tx.serialize());
    if (t.block != nullptr) {
        j["blockhash"] = t.block->hash.to_hex();
        j["blockheight"] = t.block->height;
        j["confirmations"] = node.chain.height() - t.block->height + 1;
        j["blocktime"] = t.block->header.time;
        j["position"] = t.position;
    } else if (t.entry != nullptr) {
        j["confirmations"] = 0;
        j["time"] = t.entry->time;
    } else {
        j["confirmations"] = -1;  // the wallet remembers it, but it's in neither the chain nor the mempool
    }
    if (verbosity < 2) return j;

    // What it spent: from the block's undo data, or for a waiting one from the UTXO set and the
    // mempool. Height -1 marks an output that isn't in a block yet.
    std::vector<Coin> coins;
    if (t.block != nullptr && !t.tx.is_coinbase()) {
        coins = spent_coins(node.chain, *t.block, node.chain.read_block(*t.block), t.position);
    } else if (t.entry != nullptr) {
        for (const OutPoint& in : t.tx.inputs) {
            if (const Coin* coin = node.chain.utxos().find(in)) {
                coins.push_back(*coin);
            } else if (const MempoolEntry* parent = node.mempool.find(in.txid); parent != nullptr && in.index < parent->tx.outputs.size()) {
                coins.push_back(Coin{parent->tx.outputs[in.index], -1, false});
            }
        }
    }
    if (!t.tx.is_coinbase() && coins.size() == t.tx.inputs.size()) {
        Amount in_total = 0;
        for (std::size_t i = 0; i < coins.size(); ++i) {
            in_total += coins[i].out.amount;
            json prevout = {{"address", address_of(coins[i].out.recipient, params)},
                            {"amount", format_coins(coins[i].out.amount)},
                            {"coinbase", coins[i].coinbase}};
            if (coins[i].height >= 0) prevout["height"] = coins[i].height;
            j["vin"][i]["prevout"] = std::move(prevout);
        }
        Amount out_total = 0;
        for (const TxOut& out : t.tx.outputs) out_total += out.amount;
        j["fee"] = format_coins(in_total - out_total);
        j["fee_rate"] = units_per_byte(FeeRate::of(in_total - out_total, t.tx.serialize().size()));
    }

    for (std::uint32_t n = 0; n < t.tx.outputs.size(); ++n) {
        const OutPoint op{txid, n};
        json& out = j["vout"][n];
        if (t.block != nullptr && t.block->height == 0) {
            out["spent"] = false;
            out["spendable"] = false;  // the genesis coinbase never entered the UTXO set
            continue;
        }
        const bool spent = t.block != nullptr && !node.chain.utxos().contains(op);
        out["spent"] = spent;
        if (spent) {
            if (node.index != nullptr) {
                if (const std::optional<uint256> by = node.index->spender(op)) out["spent_by"] = by->to_hex();
            }
        } else if (const MempoolEntry* by = node.mempool.spender(op)) {
            out["spent_by_unconfirmed"] = by->txid.to_hex();
        }
    }
    return j;
}

}  // namespace

json transaction_to_json(const Transaction& tx, const ConsensusParams& params) {
    json vin = json::array();
    for (const OutPoint& in : tx.inputs) vin.push_back({{"txid", in.txid.to_hex()}, {"vout", in.index}});
    json vout = json::array();
    for (std::size_t i = 0; i < tx.outputs.size(); ++i) {
        vout.push_back({{"n", i}, {"amount", format_coins(tx.outputs[i].amount)}, {"address", address_of(tx.outputs[i].recipient, params)}});
    }
    json j = {
        {"txid", tx.txid().to_hex()},
        {"wtxid", tx.wtxid().to_hex()},
        {"version", tx.version},
        {"size", tx.serialize().size()},
        {"locktime", tx.lock_time},
        {"coinbase", tx.is_coinbase()},
        {"vin", vin},
        {"vout", vout},
        {"memo", to_hex(tx.memo)},
    };
    if (!tx.memo.empty() && printable(tx.memo)) j["memo_text"] = std::string(tx.memo.begin(), tx.memo.end());
    return j;
}

void register_node_methods(RpcTable& t, NodeContext& node) {
    const ConsensusParams& params = node.chain.params();
    const std::size_t block_budget = params.max_block_size - 1000;  // what a block leaves for transactions

    // Control.
    t.add("help", "[method]", "Lists every method, or explains one.", 0, 1, [&t](const RpcParams& p) -> json {
        return p.has(0) ? t.help(p.str(0)) : t.help();
    });
    t.add("stop", "", "Stops the node.", 0, 0, [&node](const RpcParams&) -> json {
        if (node.request_stop) node.request_stop();
        return "BOUND node stopping";
    });

    // Blockchain.
    t.add("getblockchaininfo", "", "The state of the chain: height, tip, difficulty, supply.", 0, 0, [&node](const RpcParams&) -> json {
        const Chainstate& c = node.chain;
        const BlockIndex& tip = c.tip();
        return {
            {"chain", std::string(c.params().network_name)},
            {"blocks", tip.height},
            {"headers", c.best_header().height},
            {"bestblockhash", tip.hash.to_hex()},
            {"time", tip.header.time},  // the tip's timestamp
            {"difficulty", difficulty(tip.header.bits, c.params())},
            {"mediantime", tip.median_time_past(c.params().median_time_span)},
            {"chainwork", tip.chain_work.to_uint256().to_hex()},
            {"utxos", c.utxos().size()},
            {"supply", format_coins(c.utxos().total_value())},
        };
    });
    t.add("getblockcount", "", "The height of the active chain.", 0, 0, [&node](const RpcParams&) -> json { return node.chain.height(); });
    t.add("getbestblockhash", "", "The tip's hash.", 0, 0, [&node](const RpcParams&) -> json { return node.chain.tip().hash.to_hex(); });
    t.add("getblockhash", "<height>", "The hash of the active chain's block at a height.", 1, 1, [&node](const RpcParams& p) -> json {
        const std::int64_t h = p.integer(0);
        if (h < 0 || h > node.chain.height()) throw RpcError(rpc_error::kInvalidParameter, "block height out of range");
        return node.chain.at(static_cast<int>(h)).hash.to_hex();
    });
    t.add("getblockheader", "<blockhash>", "A block's header, on any branch.", 1, 1, [&node](const RpcParams& p) -> json {
        return header_to_json(find_block(node.chain, p.str(0)), node.chain);
    });
    t.add("getblock", "<blockhash> [verbosity]",
          "A block: 0 for hex, 1 (default) for its header and txids, 2 for its transactions decoded, each with its fee.", 1, 2,
          [&node](const RpcParams& p) -> json {
              const std::string hash = p.str(0);  // not a temporary: GCC would think n might refer to it
              const BlockIndex& n = find_block(node.chain, hash);
              if (!n.have_data) throw RpcError(rpc_error::kMisc, "only the block's header is here, not its transactions");
              const Block block = node.chain.read_block(n);
              const int verbosity = p.verbosity(1, 1, 2);
              if (verbosity == 0) return to_hex(block.serialize());
              json j = header_to_json(n, node.chain);
              j["size"] = block.serialize().size();
              json txs = json::array();
              // Fees need what each transaction spent: the undo data, there for any block that was ever connected.
              const std::optional<BlockUndo> undo = verbosity >= 2 ? node.chain.read_undo(n) : std::nullopt;
              std::size_t next_spent = 0;
              for (const Transaction& tx : block.txs) {
                  if (verbosity < 2) {
                      txs.push_back(tx.txid().to_hex());
                      continue;
                  }
                  json entry = transaction_to_json(tx, node.chain.params());
                  if (undo && !tx.is_coinbase() && next_spent + tx.inputs.size() <= undo->spent.size()) {
                      Amount fee = 0;
                      for (std::size_t i = 0; i < tx.inputs.size(); ++i) fee += undo->spent[next_spent + i].out.amount;
                      for (const TxOut& out : tx.outputs) fee -= out.amount;
                      entry["fee"] = format_coins(fee);
                  }
                  next_spent += tx.inputs.size();
                  txs.push_back(std::move(entry));
              }
              j["tx"] = std::move(txs);
              return j;
          });

    // Transactions and the mempool.
    t.add("getrawtransaction", "<txid> [verbosity]",
          "A transaction: 0 (default) for hex, 1 decoded with where it is, 2 also with what it spent, its fee and which "
          "outputs are spent. Looks in the mempool, then the index (any confirmed transaction, if the node runs with "
          "--index), then the wallet.",
          1, 2, [&node](const RpcParams& p) -> json {
              const uint256 txid = parse_hash(p.str(0), "txid");
              const std::optional<LocatedTx> located = locate_transaction(node, txid);
              if (!located) {
                  throw RpcError(rpc_error::kInvalidAddressOrKey, node.index != nullptr
                                                                      ? "no such transaction"
                                                                      : "no such transaction in the mempool or the wallet (the node keeps no index: --index)");
              }
              const int verbosity = p.verbosity(1, 0, 2);
              if (verbosity == 0) return to_hex(located->tx.serialize());
              return detailed_transaction(node, *located, verbosity);
          });
    t.add("gettxout", "<txid> <n> [include_mempool]",
          "An unspent output, or null if it's spent or was never there. Unless include_mempool is false, an output a "
          "waiting transaction spends counts as spent, and one a waiting transaction made counts as there.",
          2, 3, [&node](const RpcParams& p) -> json {
              const uint256 txid = parse_hash(p.str(0), "txid");
              const std::int64_t n = p.integer(1);
              if (n < 0 || n > 0xffffffffLL) throw RpcError(rpc_error::kInvalidParameter, "n is out of range");
              const bool with_mempool = !p.has(2) || p.boolean(2);
              const OutPoint op{txid, static_cast<std::uint32_t>(n)};
              const ConsensusParams& cp = node.chain.params();
              if (with_mempool && node.mempool.spender(op) != nullptr) return nullptr;
              const auto describe = [&](const TxOut& out, int confirmations, bool coinbase) -> json {
                  return {{"bestblock", node.chain.tip().hash.to_hex()},
                          {"confirmations", confirmations},
                          {"amount", format_coins(out.amount)},
                          {"address", address_of(out.recipient, cp)},
                          {"coinbase", coinbase}};
              };
              if (const Coin* coin = node.chain.utxos().find(op)) {
                  return describe(coin->out, node.chain.height() - coin->height + 1, coin->coinbase);
              }
              if (with_mempool) {
                  const MempoolEntry* e = node.mempool.find(txid);
                  if (e != nullptr && op.index < e->tx.outputs.size()) return describe(e->tx.outputs[op.index], 0, false);
              }
              return nullptr;
          });
    t.add("decoderawtransaction", "<hex>", "Decodes a serialized transaction.", 1, 1, [&node](const RpcParams& p) -> json {
        return transaction_to_json(parse_transaction(p.str(0)), node.chain.params());
    });
    t.add("sendrawtransaction", "<hex>", "Offers a signed transaction to the mempool and the network; returns its txid.", 1, 1,
          [&node](const RpcParams& p) -> json {
              const Transaction tx = parse_transaction(p.str(0));
              const MempoolAccept r = node.peers.submit_transaction(tx, node.clock());
              if (!r.accepted() && r.result != MempoolResult::Duplicate) {
                  throw RpcError(rpc_error::kVerifyRejected, std::string(to_string(r.result)) + ": " + r.reason);
              }
              return tx.txid().to_hex();
          });
    t.add("getmempoolinfo", "", "The mempool's size and fee rates (units per byte).", 0, 0, [&node, block_budget](const RpcParams&) -> json {
        const Mempool& m = node.mempool;
        return {
            {"size", m.size()},
            {"bytes", m.bytes()},
            {"total_fee", format_coins(m.total_fee())},
            {"max_bytes", m.options().max_bytes},
            {"min_relay_fee_rate", units_per_byte(m.options().min_relay_fee)},
            {"next_block_fee_rate", units_per_byte(m.next_block_fee_rate(block_budget))},
        };
    });
    t.add("getrawmempool", "[verbose]", "The txids waiting, in arrival order; with verbose, each entry's details.", 0, 1,
          [&node](const RpcParams& p) -> json {
              const bool verbose = p.has(0) && p.boolean(0);
              json j = verbose ? json::object() : json::array();
              for (const MempoolEntry* e : node.mempool.entries()) {
                  if (verbose) {
                      j[e->txid.to_hex()] = entry_to_json(*e);
                  } else {
                      j.push_back(e->txid.to_hex());
                  }
              }
              return j;
          });
    t.add("getmempoolentry", "<txid>", "One waiting transaction's details.", 1, 1, [&node](const RpcParams& p) -> json {
        const MempoolEntry* e = node.mempool.find(parse_hash(p.str(0), "txid"));
        if (e == nullptr) throw RpcError(rpc_error::kInvalidAddressOrKey, "not in the mempool");
        return entry_to_json(*e);
    });
    t.add("estimatefee", "", "The fee rate (units per byte) to make the next block, judged by what's waiting now.", 0, 0,
          [&node, block_budget](const RpcParams&) -> json { return units_per_byte(node.mempool.next_block_fee_rate(block_budget)); });

    // Milestone 8: the indexes (--index).
    const auto paging = [](const RpcParams& p, std::size_t first) {
        const std::int64_t count = p.opt_integer(first).value_or(25);
        const std::int64_t skip = p.opt_integer(first + 1).value_or(0);
        if (count < 1 || count > 1000) throw RpcError(rpc_error::kInvalidParameter, "count must be from 1 to 1000");
        if (skip < 0) throw RpcError(rpc_error::kInvalidParameter, "skip can't be negative");
        return std::pair{static_cast<std::size_t>(count), static_cast<std::size_t>(skip)};
    };
    t.add("getindexinfo", "", "Whether this node keeps the transaction and address indexes, and how far they've got.", 0, 0,
          [&node](const RpcParams&) -> json {
              if (node.index == nullptr) return {{"enabled", false}};
              const BlockIndex* best = node.chain.find(node.index->best_block());
              return {{"enabled", true},
                      {"best_block", node.index->best_block().to_hex()},
                      {"height", best != nullptr ? best->height : -1},
                      {"synced", best == &node.chain.tip()}};
          });
    t.add("getaddressbalance", "<address>",
          "An address's confirmed totals: balance, everything received and sent, and how many transactions and unspent "
          "outputs. Needs --index.",
          1, 1, [&node](const RpcParams& p) -> json {
              const std::string address = p.str(0);
              const AddressSummary s = need_index(node).summary(parse_address(address, node.chain.params()));
              return {{"address", address},
                      {"balance", format_coins(s.balance)},
                      {"received", format_coins(s.received)},
                      {"sent", format_coins(s.sent)},
                      {"txcount", s.txs},
                      {"utxos", s.coins}};
          });
    t.add("getaddressdeltas", "<address> [count] [skip]",
          "The confirmed transactions that touch an address, newest first: what each paid it and spent from it. 25 "
          "unless count says; skip leaves out the newest ones, for paging. Needs --index.",
          1, 3, [&node, paging](const RpcParams& p) -> json {
              const PublicKey address = parse_address(p.str(0), node.chain.params());
              const auto [count, skip] = paging(p, 1);
              json list = json::array();
              for (const AddressDelta& d : need_index(node).history(address, skip, count)) {
                  list.push_back({{"txid", d.txid.to_hex()},
                                  {"height", d.height},
                                  {"position", d.position},
                                  {"time", node.chain.at(d.height).header.time},
                                  {"received", format_coins(d.received)},
                                  {"spent", format_coins(d.spent)},
                                  {"delta", format_coins(d.received - d.spent)}});
              }
              return list;
          });
    t.add("getaddressutxos", "<address> [count] [skip]", "An address's unspent outputs, newest first. Needs --index.", 1, 3,
          [&node, paging](const RpcParams& p) -> json {
              const PublicKey address = parse_address(p.str(0), node.chain.params());
              const auto [count, skip] = paging(p, 1);
              json list = json::array();
              for (const AddressCoin& c : need_index(node).coins(address, skip, count)) {
                  list.push_back({{"txid", c.op.txid.to_hex()},
                                  {"vout", c.op.index},
                                  {"amount", format_coins(c.amount)},
                                  {"height", c.height},
                                  {"confirmations", node.chain.height() - c.height + 1},
                                  {"coinbase", c.coinbase}});
              }
              return list;
          });

    // Network.
    t.add("getnetworkhashps", "[nblocks]",
          "The network's estimated hashes per second, from the work and the time of the last nblocks blocks (120 by default).", 0, 1,
          [&node](const RpcParams& p) -> json {
              const std::int64_t wanted = p.opt_integer(0).value_or(120);
              if (wanted < 1) throw RpcError(rpc_error::kInvalidParameter, "nblocks must be positive");
              const BlockIndex& tip = node.chain.tip();
              const int blocks = static_cast<int>(std::min<std::int64_t>(wanted, tip.height));
              if (blocks == 0) return 0.0;
              const BlockIndex& first = node.chain.at(tip.height - blocks);
              const double seconds = std::max(1.0, static_cast<double>(tip.header.time) - static_cast<double>(first.header.time));
              return (tip.chain_work - first.chain_work).to_double() / seconds;
          });
    t.add("getnetworkinfo", "",
          "This node's version and connections, and the addresses it may be reachable at from outside "
          "(localaddresses: from --externalip, or where its outbound peers see it; the first is the one it "
          "advertises). connections_in above 0 from public addresses means others can reach it.",
          0, 0, [&node](const RpcParams&) -> json {
              const std::vector<PeerInfo> peers = node.peers.peers();
              const auto in = static_cast<std::size_t>(std::count_if(peers.begin(), peers.end(), [](const PeerInfo& i) { return i.inbound; }));
              json local = json::array();
              for (const LocalAddress& l : node.peers.local_addresses()) {
                  local.push_back({{"address", l.addr.ip_string()}, {"port", l.addr.port},
                                   {"source", l.manual ? "externalip" : "peers"}, {"peers", l.peers}});
              }
              return {
                  {"subversion", node.user_agent},
                  {"protocolversion", kProtocolVersion},
                  {"connections", peers.size()},
                  {"connections_in", in},
                  {"connections_out", peers.size() - in},
                  {"port", node.p2p_port},
                  {"localaddresses", local},
                  {"headers_presyncs", node.peers.headers_syncs()},
              };
          });
    t.add("getconnectioncount", "", "How many peers are connected.", 0, 0, [&node](const RpcParams&) -> json { return node.peers.peers().size(); });
    t.add("getpeerinfo", "", "Every connected peer.", 0, 0, [&node](const RpcParams&) -> json {
        json list = json::array();
        for (const PeerInfo& info : node.peers.peers()) {
            list.push_back({
                {"id", info.id},
                {"addr", info.addr.to_string()},
                {"inbound", info.inbound},
                {"handshake_done", info.ready},
                {"start_height", info.start_height},
                {"subver", info.user_agent},
                {"blocks_in_flight", info.blocks_in_flight},
            });
        }
        return list;
    });

    // Hardening: bans.
    t.add("setban", "<ip> <add|remove> [seconds]",
          "Bans an IP address (for a day unless seconds says otherwise), or lifts a ban. A banned address is "
          "disconnected, never dialled and never let in. Peers that break the rules are discouraged for a day on "
          "their own; that list isn't shown here.",
          2, 3, [&node](const RpcParams& p) -> json {
              if (node.banman == nullptr) throw RpcError(rpc_error::kMisc, "bans aren't available here");
              const std::string text = p.str(0);
              const std::vector<NetAddress> found = resolve(text, 1);
              if (found.empty()) throw RpcError(rpc_error::kInvalidParameter, "not an IP address: " + text);
              const std::string action = p.str(1);
              if (action == "add") {
                  const std::int64_t seconds = p.opt_integer(2).value_or(BanMan::kDiscourageTime);
                  if (seconds <= 0) throw RpcError(rpc_error::kInvalidParameter, "seconds must be positive");
                  node.banman->ban(found.front(), node.clock() + seconds);
                  node.peers.disconnect_ip(found.front());
              } else if (action == "remove") {
                  if (!node.banman->unban(found.front())) throw RpcError(rpc_error::kMisc, text + " isn't banned");
              } else {
                  throw RpcError(rpc_error::kInvalidParameter, "the second parameter must be add or remove");
              }
              if (node.save_bans) node.save_bans();
              return nullptr;
          });
    t.add("listbanned", "", "The banned addresses, and when each ban ends.", 0, 0, [&node](const RpcParams&) -> json {
        if (node.banman == nullptr) throw RpcError(rpc_error::kMisc, "bans aren't available here");
        json list = json::array();
        const std::int64_t now = node.clock();
        for (const auto& [addr, until] : node.banman->banned(now)) {
            list.push_back({{"address", addr.ip_string()}, {"banned_until", until}, {"ban_remaining", until - now}});
        }
        return list;
    });
    t.add("clearbanned", "", "Lifts every ban.", 0, 0, [&node](const RpcParams&) -> json {
        if (node.banman == nullptr) throw RpcError(rpc_error::kMisc, "bans aren't available here");
        node.banman->clear_banned();
        if (node.save_bans) node.save_bans();
        return nullptr;
    });

    // Mining.
    t.add("generatetoaddress", "<nblocks> <address>",
          "Mines blocks right away, with what's in the mempool, paying the address; returns their hashes. Blocks the node "
          "while it works.",
          2, 2, [&node, block_budget](const RpcParams& p) -> json {
              const std::int64_t count = p.integer(0);
              if (count < 1 || count > 1000) throw RpcError(rpc_error::kInvalidParameter, "nblocks must be from 1 to 1000");
              const PublicKey payee = parse_address(p.str(1), node.chain.params());
              json hashes = json::array();
              for (std::int64_t i = 0; i < count; ++i) {
                  const auto now = static_cast<std::uint32_t>(node.clock());
                  std::optional<Block> tmpl = make_block_template(node.chain, payee, node.mempool.select(block_budget).txs, now);
                  if (!tmpl) tmpl = make_block_template(node.chain, payee, {}, now);
                  const Block block = mine_block(std::move(tmpl.value()), node.chain.params());
                  if (!node.peers.submit_block(block, node.clock()).accepted()) throw RpcError(rpc_error::kMisc, "a mined block was rejected");
                  hashes.push_back(block.hash().to_hex());
              }
              return hashes;
          });
    // Milestone 9: mining in the background, without blocking the node, switched on and off at will.
    const auto mining_info = [&node] {
        const ConsensusParams& params = node.chain.params();
        const MiningControl* m = node.mining;
        const bool on = m != nullptr && m->payee.has_value();
        return json{
            {"mining", on},
            {"status", !on ? "off" : m->paused ? "waiting for the initial sync" : "mining"},
            {"address", on ? json(address_of(*m->payee, params)) : json(nullptr)},
            {"hashespersec", on ? m->hashes_per_second : 0.0},
            {"threads", m != nullptr && m->threads != 0 ? m->threads : BackgroundMiner::default_threads()},
            {"blocksfound", m != nullptr ? m->blocks_found : 0},
            {"blocks", node.chain.height()},
            {"difficulty", difficulty(next_work_required(node.chain.tip(), params), params)},
            {"pooledtx", node.mempool.size()},
        };
    };
    t.add("getmininginfo", "", "Whether this node mines in the background (setmining), how fast, and the next block's difficulty.", 0, 0,
          [mining_info](const RpcParams&) -> json { return mining_info(); });
    t.add("setmining", "<true|false> [address] [threads]",
          "Starts mining in the background, paying the address (by default a new one from the wallet), or stops. threads: "
          "how many of the processor's threads to use (by default all but one). The node keeps working meanwhile; mined "
          "blocks are announced like any other.",
          1, 3, [&node, mining_info](const RpcParams& p) -> json {
              if (node.mining == nullptr) throw RpcError(rpc_error::kMisc, "background mining isn't available here");
              if (!p.boolean(0)) {
                  node.mining->payee.reset();
                  return mining_info();
              }
              if (p.has(2)) {
                  const std::int64_t threads = p.integer(2);
                  if (threads < 1 || threads > 1024) throw RpcError(rpc_error::kInvalidParameter, "threads must be from 1 to 1024");
                  node.mining->threads = static_cast<unsigned>(threads);
              }
              if (p.has(1)) {
                  node.mining->payee = parse_address(p.str(1), node.chain.params());
              } else {
                  try {
                      node.mining->payee = need_wallet(node).new_address();
                  } catch (const WalletError& e) {
                      throw RpcError(code_for(e.kind), e.what());
                  }
              }
              return mining_info();
          });

    // Utility.
    t.add("validateaddress", "<address>", "Whether an address is valid on this network, and whether it's the wallet's.", 1, 1,
          [&node](const RpcParams& p) -> json {
              const std::string text = p.str(0);
              const std::optional<PublicKey> key = decode_address(text, node.chain.params().address_version);
              if (!key) return {{"isvalid", false}};
              return {{"isvalid", true}, {"address", text}, {"pubkey", to_hex(*key)}, {"ismine", node.wallet && node.wallet->is_mine(*key)}};
          });

    // Wallet.
    t.add("createwallet", "<passphrase>",
          "Creates the wallet, encrypted with the passphrase, and returns its seed as 24 words (and as hex): write the "
          "words down, they restore every key.",
          1, 1, wallet_method([&node](const RpcParams& p) -> json {
              if (node.wallet_db == nullptr) throw RpcError(rpc_error::kWallet, "this node has nowhere to keep a wallet");
              if (node.wallet) throw RpcError(rpc_error::kWallet, "there's a wallet already");
              const std::string passphrase = p.str(0);
              if (passphrase.empty()) throw RpcError(rpc_error::kInvalidParameter, "the passphrase can't be empty");
              Seed seed = random_seed();
              node.wallet = Wallet::create(*node.wallet_db, node.chain, node.mempool, passphrase, seed, false, node.wallet_options);
              json result = seed_json(seed);
              sodium_memzero(seed.data(), seed.size());
              result["warning"] = "Anyone with these words can spend the wallet's coins; without them a lost wallet can't be restored.";
              return result;
          }));
    t.add("restorewallet", "<words> <passphrase>",
          "Creates the wallet from its seed (the 24 words in one argument, or the 64 hex digits versions before 1.0 "
          "showed) and scans the whole chain for its coins.",
          2, 2, wallet_method([&node](const RpcParams& p) -> json {
              if (node.wallet_db == nullptr) throw RpcError(rpc_error::kWallet, "this node has nowhere to keep a wallet");
              if (node.wallet) throw RpcError(rpc_error::kWallet, "there's a wallet already");
              std::string problem;
              std::optional<Seed> parsed = parse_seed(p.str(0), &problem);
              if (!parsed) throw RpcError(rpc_error::kInvalidParameter, "the seed: " + problem);
              const std::string passphrase = p.str(1);
              if (passphrase.empty()) {
                  sodium_memzero(parsed->data(), parsed->size());
                  throw RpcError(rpc_error::kInvalidParameter, "the passphrase can't be empty");
              }
              Seed seed = *parsed;
              sodium_memzero(parsed->data(), parsed->size());
              node.wallet = Wallet::create(*node.wallet_db, node.chain, node.mempool, passphrase, seed, true, node.wallet_options);
              sodium_memzero(seed.data(), seed.size());
              return {{"transactions", node.wallet->transaction_count()}, {"balance", format_coins(node.wallet->balance().spendable)}};
          }));
    t.add("getwalletinfo", "", "Balances, transaction count, keypool and lock state.", 0, 0, wallet_method([&node](const RpcParams&) -> json {
        const Wallet& w = need_wallet(node);
        const WalletBalance b = w.balance();
        return {
            {"balance", format_coins(b.spendable)},
            {"unconfirmed_balance", format_coins(b.unconfirmed)},
            {"immature_balance", format_coins(b.immature)},
            {"txcount", w.transaction_count()},
            {"keypoolsize", w.keypool_size()},
            {"locked", w.locked()},
            {"unlocked_until", w.unlocked_until()},
        };
    }));
    t.add("getbalance", "", "What the wallet can spend now: confirmed, mature coins plus its own unconfirmed change.", 0, 0,
          wallet_method([&node](const RpcParams&) -> json { return format_coins(need_wallet(node).balance().spendable); }));
    t.add("getnewaddress", "", "A new address to receive payments.", 0, 0, wallet_method([&node](const RpcParams&) -> json {
        return address_of(need_wallet(node).new_address(), node.chain.params());
    }));
    t.add("listunspent", "", "The wallet's unspent outputs.", 0, 0, wallet_method([&node](const RpcParams&) -> json {
        json list = json::array();
        for (const WalletCoin& c : need_wallet(node).coins()) {
            list.push_back({
                {"txid", c.op.txid.to_hex()},
                {"vout", c.op.index},
                {"address", address_of(c.out.recipient, node.chain.params())},
                {"amount", format_coins(c.out.amount)},
                {"confirmations", c.confirmations},
                {"spendable", c.spendable},
                {"coinbase", c.coinbase},
                {"change", c.change},
            });
        }
        return list;
    }));
    t.add("listtransactions", "[count]", "The wallet's last transactions (10 unless count says), oldest first.", 0, 1,
          wallet_method([&node](const RpcParams& p) -> json {
              const Wallet& w = need_wallet(node);
              const std::int64_t count = p.opt_integer(0).value_or(10);
              if (count < 0) throw RpcError(rpc_error::kInvalidParameter, "count can't be negative");
              const std::vector<WalletTxSummary> all = w.transactions();
              const std::size_t first = all.size() - std::min(all.size(), static_cast<std::size_t>(count));
              json list = json::array();
              for (std::size_t i = first; i < all.size(); ++i) list.push_back(summary_to_json(all[i], w, node.chain.params()));
              return list;
          }));
    t.add("gettransaction", "<txid>", "One wallet transaction in detail.", 1, 1, wallet_method([&node](const RpcParams& p) -> json {
        const Wallet& w = need_wallet(node);
        const WalletTx* wtx = w.find(parse_hash(p.str(0), "txid"));
        if (wtx == nullptr) throw RpcError(rpc_error::kInvalidAddressOrKey, "not a wallet transaction");
        json j = summary_to_json(w.summarize(*wtx), w, node.chain.params());
        json details = json::array();
        for (std::size_t i = 0; i < wtx->tx.outputs.size(); ++i) {
            const TxOut& out = wtx->tx.outputs[i];
            details.push_back({{"n", i}, {"address", address_of(out.recipient, node.chain.params())}, {"amount", format_coins(out.amount)},
                               {"mine", w.is_mine(out.recipient)}});
        }
        j["details"] = details;
        j["hex"] = to_hex(wtx->tx.serialize());
        return j;
    }));
    t.add("sendtoaddress", "<address> <amount> [fee_rate] [memo]",
          "Pays an amount of BOUND to an address and returns the txid. fee_rate is in units per byte; by default, "
          "whatever makes the next block. Needs the wallet unlocked.",
          2, 4, wallet_method([&node, block_budget](const RpcParams& p) -> json {
              Wallet& w = need_wallet(node);
              const PublicKey to = parse_address(p.str(0), node.chain.params());
              const Amount amount = p.amount(1);
              const FeeRate rate = p.has(2) ? parse_fee_rate(p, 2) : node.mempool.next_block_fee_rate(block_budget);
              const std::string memo = p.opt_str(3).value_or("");
              const Transaction tx = w.create_payment(to, amount, rate, bytes_of(memo));
              const MempoolAccept r = node.peers.submit_transaction(tx, node.clock());
              if (!r.accepted()) throw RpcError(rpc_error::kWallet, "the mempool refused the payment: " + r.reason);
              return tx.txid().to_hex();
          }));
    // Milestone 18: the wallet app's Max.
    t.add("sendall", "<address> [fee_rate] [memo]",
          "Pays everything the wallet can spend to an address, the fee taken out of it, and returns the txid, the "
          "amount sent and the fee. fee_rate as for sendtoaddress. Needs the wallet unlocked.",
          1, 3, wallet_method([&node, block_budget](const RpcParams& p) -> json {
              Wallet& w = need_wallet(node);
              const PublicKey to = parse_address(p.str(0), node.chain.params());
              const FeeRate rate = p.has(1) ? parse_fee_rate(p, 1) : node.mempool.next_block_fee_rate(block_budget);
              const std::string memo = p.opt_str(2).value_or("");
              const Transaction tx = w.create_sweep(to, rate, bytes_of(memo));
              const MempoolAccept r = node.peers.submit_transaction(tx, node.clock());
              if (!r.accepted()) throw RpcError(rpc_error::kWallet, "the mempool refused the payment: " + r.reason);
              const MempoolEntry* entry = node.mempool.find(tx.txid());
              return {{"txid", tx.txid().to_hex()},
                      {"amount", format_coins(tx.outputs.at(0).amount)},
                      {"fee", format_coins(entry != nullptr ? entry->fee : 0)}};
          }));
    t.add("bumpfee", "<txid> [fee_rate]",
          "Replaces one of the wallet's unconfirmed transactions with one paying a higher fee (full RBF). fee_rate is in "
          "units per byte; by default, whatever makes the next block (and at least 1 more than the original). Needs the "
          "wallet unlocked.",
          1, 2, wallet_method([&node, block_budget](const RpcParams& p) -> json {
              Wallet& w = need_wallet(node);
              const uint256 txid = parse_hash(p.str(0), "txid");
              const MempoolEntry* original = node.mempool.find(txid);
              const Amount original_fee = original != nullptr ? original->fee : 0;
              FeeRate rate = p.has(1) ? parse_fee_rate(p, 1) : node.mempool.next_block_fee_rate(block_budget);
              if (!p.has(1) && original != nullptr) {
                  rate = std::max(rate, FeeRate{FeeRate::of(original->fee, original->size).per_kb + 1000});
              }
              const Transaction tx = w.create_bump(txid, rate);
              const MempoolAccept r = node.peers.submit_transaction(tx, node.clock());
              if (!r.accepted()) throw RpcError(rpc_error::kWallet, "the mempool refused the replacement: " + r.reason);
              return {{"txid", tx.txid().to_hex()}, {"origfee", format_coins(original_fee)}, {"fee", format_coins(r.fee)}};
          }));
    t.add("walletpassphrase", "<passphrase> <timeout>", "Unlocks the wallet for timeout seconds, for spending.", 2, 2,
          wallet_method([&node](const RpcParams& p) -> json {
              Wallet& w = need_wallet(node);
              const std::int64_t timeout = p.integer(1);
              if (timeout <= 0 || timeout > 100'000'000) throw RpcError(rpc_error::kInvalidParameter, "timeout must be from 1 to 100000000 seconds");
              w.unlock(p.str(0), node.clock() + timeout);
              return nullptr;
          }));
    t.add("walletlock", "", "Locks the wallet: forgets the seed until the next walletpassphrase.", 0, 0,
          wallet_method([&node](const RpcParams&) -> json {
              need_wallet(node).lock();
              return nullptr;
          }));
    t.add("walletpassphrasechange", "<old> <new>", "Changes the wallet's passphrase.", 2, 2, wallet_method([&node](const RpcParams& p) -> json {
        const std::string next = p.str(1);
        if (next.empty()) throw RpcError(rpc_error::kInvalidParameter, "the passphrase can't be empty");
        need_wallet(node).change_passphrase(p.str(0), next);
        return nullptr;
    }));
    t.add("dumpseed", "", "The wallet's seed, the backup of every key: its 24 words, and the same seed as hex. Needs the wallet unlocked.",
          0, 0, wallet_method([&node](const RpcParams&) -> json {
              Seed seed = need_wallet(node).seed();
              json result = seed_json(seed);
              sodium_memzero(seed.data(), seed.size());
              return result;
          }));
}

}  // namespace bound
