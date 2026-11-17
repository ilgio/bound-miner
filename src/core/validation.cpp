#include "core/validation.h"

#include "core/chaindb.h"
#include "core/consensus.h"
#include "core/pow.h"

#include <algorithm>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace bound {

InputsCheck check_tx_inputs(const Transaction& tx, const UtxoSet& utxos, int spend_height,
                            const ConsensusParams& params, Amount* fee) {
    std::vector<const Coin*> coins;
    coins.reserve(tx.inputs.size());
    for (const OutPoint& in : tx.inputs) {
        const Coin* coin = utxos.find(in);
        if (coin == nullptr) return InputsCheck::MissingInput;
        coins.push_back(coin);
    }
    for (const Coin* coin : coins) {
        if (coin->coinbase && spend_height - coin->height < params.coinbase_maturity) return InputsCheck::ImmatureCoinbase;
    }
    Amount in_total = 0;
    for (const Coin* coin : coins) {
        if (!money_range(coin->out.amount)) return InputsCheck::InputValueOutOfRange;
        in_total += coin->out.amount;
        if (!money_range(in_total)) return InputsCheck::InputValueOutOfRange;
    }
    Amount out_total = 0;  // check_transaction already bounded every output and their sum
    for (const TxOut& out : tx.outputs) out_total += out.amount;
    if (out_total > in_total) return InputsCheck::InputsBelowOutputs;
    for (std::size_t i = 0; i < coins.size(); ++i) {
        if (!verify_input(tx, i, coins[i]->out.recipient)) return InputsCheck::BadSignature;
    }
    if (fee != nullptr) *fee = in_total - out_total;
    return InputsCheck::Ok;
}

namespace {

// Every change connect_block makes, so a failure halfway through can be rolled back exactly.
struct Change {
    bool added;  // true: `op` was added; false: `op` (holding `coin`) was spent
    OutPoint op;
    Coin coin;
};

void roll_back(const std::vector<Change>& log, UtxoSet& utxos) {
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (it->added) {
            utxos.spend(it->op);
        } else {
            utxos.add(it->op, it->coin);
        }
    }
}

}  // namespace

ConnectResult connect_block(const Block& block, int height, UtxoSet& utxos, BlockUndo& undo,
                            const ConsensusParams& params) {
    // The header's proof of work was checked when it entered the block tree (milestone 16).
    if (check_block(block, params, false) != BlockCheck::Ok) return ConnectResult::BadBlock;
    if (block.txs[0].lock_time != static_cast<std::uint32_t>(height)) return ConnectResult::BadCoinbaseHeight;

    std::vector<Change> log;
    Amount fees = 0;
    for (const Transaction& tx : block.txs) {
        if (!tx.is_coinbase()) {
            Amount fee = 0;
            if (check_tx_inputs(tx, utxos, height, params, &fee) != InputsCheck::Ok) {
                roll_back(log, utxos);
                return ConnectResult::BadInputs;
            }
            fees += fee;  // each fee <= its inputs, and all inputs together <= the supply: no overflow
            for (const OutPoint& in : tx.inputs) {
                Coin coin = *utxos.spend(in);
                log.push_back({false, in, std::move(coin)});
            }
        }
        const uint256 txid = tx.txid();
        for (std::uint32_t i = 0; i < tx.outputs.size(); ++i) {
            const OutPoint op{txid, i};
            if (!utxos.add(op, Coin{tx.outputs[i], height, tx.is_coinbase()})) {
                roll_back(log, utxos);
                return ConnectResult::DuplicateOutput;
            }
            log.push_back({true, op, {}});
        }
    }

    Amount claimed = 0;
    for (const TxOut& out : block.txs[0].outputs) claimed += out.amount;
    if (claimed > block_subsidy(height, params) + fees) {
        roll_back(log, utxos);
        return ConnectResult::CoinbaseTooLarge;
    }

    undo.spent.clear();
    for (Change& change : log) {
        if (!change.added) undo.spent.push_back(std::move(change.coin));
    }
    return ConnectResult::Ok;
}

void disconnect_block(const Block& block, const BlockUndo& undo, UtxoSet& utxos) {
    std::size_t next = undo.spent.size();
    for (auto tx = block.txs.rbegin(); tx != block.txs.rend(); ++tx) {
        const uint256 txid = tx->txid();
        for (std::uint32_t i = 0; i < tx->outputs.size(); ++i) utxos.spend(OutPoint{txid, i});
        for (auto in = tx->inputs.rbegin(); in != tx->inputs.rend(); ++in) {
            if (next == 0) throw std::logic_error("undo data doesn't match the block");
            utxos.add(*in, undo.spent[--next]);
        }
    }
    if (next != 0) throw std::logic_error("undo data doesn't match the block");
}

namespace {

void put_index(WriteBatch& batch, const BlockIndex& node) { batch.put(index_key(node.hash), encode_index(node)); }

std::runtime_error damaged(const std::string& what) {
    return std::runtime_error("the stored chain is damaged: " + what);
}

}  // namespace

bool Chainstate::MoreWork::operator()(const BlockIndex* a, const BlockIndex* b) const {
    if (a->chain_work != b->chain_work) return a->chain_work > b->chain_work;
    return a->sequence < b->sequence;
}

Chainstate::Chainstate(const ConsensusParams& params)
    : params_(params),
      owned_db_(std::make_unique<MemoryStore>()),
      owned_blocks_(std::make_unique<MemoryBlockStore>()),
      db_(*owned_db_),
      block_store_(*owned_blocks_) {
    open();
}

Chainstate::Chainstate(const ConsensusParams& params, KeyValueStore& db, BlockStore& blocks)
    : params_(params), db_(db), block_store_(blocks) {
    open();
}

void Chainstate::open() {
    if (db_.get(meta_key())) {
        load();
    } else {
        create();
    }
    activate_best_chain();  // a crash may have stopped short of the best chain
}

// One write, after the genesis block itself is stored: a crash before it leaves storage that
// opens as empty again.
void Chainstate::create() {
    const Block genesis = genesis_block(params_);
    const uint256 hash = genesis.hash();
    BlockIndex& node = tree_[hash];
    node.hash = hash;
    node.header = genesis.header;
    node.chain_work = block_proof(genesis.header.bits);
    node.pos = block_store_.append(genesis);
    node.have_data = true;

    WriteBatch batch;
    put_index(batch, node);
    link(node, batch);
    batch.put(meta_key(), encode_meta({kChainDbVersion, hash, std::string(params_.network_name)}));
    batch.put(tip_key(), encode_hash(hash));
    db_.write(batch);
    active_.push_back(&node);
    best_header_ = &node;
}

void Chainstate::load() {
    const ChainMeta meta = decode_meta(*db_.get(meta_key()));
    if (meta.version != kChainDbVersion) throw std::runtime_error(std::format("unknown storage version {}", meta.version));
    if (meta.genesis != params_.genesis.hash()) {
        // Milestone 15: the same network with another genesis is a chain its genesis replaced
        // (0.10.0's --mainnet ran a provisional one; milestone 16 started the testnet over). Nothing
        // on it carries over. Milestone 16: `boundd node` and bound-qt move it aside by themselves
        // (set_aside_replaced_chain); this is for everything else that opens a data directory.
        if (meta.network == params_.network_name) {
            throw std::runtime_error(std::format(
                "the stored {} chain starts from another genesis block ({}), one this version replaced: "
                "`boundd node` and bound-qt set such a chain aside by themselves, or move this data directory aside",
                meta.network, meta.genesis.to_hex()));
        }
        throw std::runtime_error("the stored chain belongs to another network (" + meta.network + ")");
    }

    // The tree: parents before children, so each node can link to its parent and add up its work.
    std::vector<BlockIndex> stored;
    db_.for_each(ByteSpan(&kIndexPrefix, 1), [&](ByteSpan key, ByteSpan value) {
        stored.push_back(decode_index(value));
        if (index_key(stored.back().hash) != Bytes(key.begin(), key.end())) throw damaged("an index key doesn't match its header");
    });
    std::sort(stored.begin(), stored.end(),
              [](const BlockIndex& a, const BlockIndex& b) { return std::tie(a.height, a.hash) < std::tie(b.height, b.hash); });
    for (const BlockIndex& record : stored) {
        BlockIndex& node = tree_.emplace(record.hash, record).first->second;
        if (node.height == 0) {
            if (node.hash != meta.genesis) throw damaged("a second block at height 0");
            node.chain_work = block_proof(node.header.bits);
        } else {
            const auto parent = tree_.find(node.header.prev_hash);
            if (parent == tree_.end() || parent->second.height != node.height - 1) throw damaged("a block without its parent");
            node.parent = &parent->second;
            node.parent->children.push_back(&node);
            node.chain_work = node.parent->chain_work + block_proof(node.header.bits);
        }
        node.linked = node.have_data && (node.parent == nullptr || node.parent->linked);
        if (node.linked && !node.invalid) candidates_.insert(&node);
        next_sequence_ = std::max(next_sequence_, node.sequence + 1);
    }
    find_best_header();

    const std::optional<Bytes> tip_hash = db_.get(tip_key());
    const auto tip = tip_hash ? tree_.find(decode_hash(*tip_hash)) : tree_.end();
    if (tip == tree_.end() || !tip->second.linked || tip->second.invalid) throw damaged("no valid tip");
    active_.resize(static_cast<std::size_t>(tip->second.height) + 1);
    for (BlockIndex* n = &tip->second; n != nullptr; n = n->parent) active_[static_cast<std::size_t>(n->height)] = n;

    db_.for_each(ByteSpan(&kCoinPrefix, 1), [&](ByteSpan key, ByteSpan value) {
        if (!utxos_.add(decode_coin_key(key), decode_coin(value))) throw damaged("a coin stored twice");
    });

    verify_recent_blocks(6);
}

// Re-reads the last `count` blocks of the active chain and their undo data, so that damaged files
// are found at startup rather than in the middle of a reorg.
void Chainstate::verify_recent_blocks(int count) const {
    for (int h = std::max(1, height() - count + 1); h <= height(); ++h) {
        bool ok = false;
        try {
            const Block block = read_block(at(h));
            const std::optional<Bytes> undo = db_.get(undo_key(at(h).hash));
            std::size_t inputs = 0;
            for (const Transaction& tx : block.txs) inputs += tx.inputs.size();
            ok = check_block(block, params_, false) == BlockCheck::Ok && undo && decode_undo(*undo).spent.size() == inputs;
        } catch (const std::runtime_error&) {
            // reported below
        }
        if (!ok) throw damaged(std::format("block #{} or its undo data", h));
    }
}

void Chainstate::find_best_header() {
    best_header_ = nullptr;
    for (auto& [hash, node] : tree_) {
        if (!node.invalid && (best_header_ == nullptr || node.chain_work > best_header_->chain_work)) best_header_ = &node;
    }
}

Block Chainstate::read_block(const BlockIndex& node) const {
    Block block = block_store_.read(node.pos);
    if (block.hash() != node.hash) throw damaged(std::format("the block stored for #{} is a different block", node.height));
    return block;
}

std::optional<BlockUndo> Chainstate::read_undo(const BlockIndex& node) const {
    const std::optional<Bytes> stored = db_.get(undo_key(node.hash));
    if (!stored) return std::nullopt;
    return decode_undo(*stored);
}

AcceptResult Chainstate::check_header(const BlockHeader& header, std::int64_t now) const {
    const uint256 hash = header.hash();
    if (tree_.contains(hash)) return AcceptResult::Duplicate;
    if (!check_proof_of_work(header, params_)) return AcceptResult::BadProofOfWork;
    const auto found = tree_.find(header.prev_hash);
    if (found == tree_.end()) return AcceptResult::UnknownParent;
    const BlockIndex& parent = found->second;
    if (parent.invalid) return AcceptResult::InvalidParent;
    if (header.bits != next_work_required(parent, params_)) return AcceptResult::BadDifficulty;
    if (header.time <= parent.median_time_past(params_.median_time_span)) return AcceptResult::TimeTooOld;
    if (static_cast<std::int64_t>(header.time) > now + params_.max_future_block_time) return AcceptResult::TimeTooNew;
    return AcceptResult::Accepted;
}

// Precondition: check_header(header) returned Accepted.
BlockIndex& Chainstate::add_header(const BlockHeader& header, WriteBatch& batch) {
    BlockIndex& parent = tree_.at(header.prev_hash);
    const uint256 hash = header.hash();
    BlockIndex& node = tree_[hash];  // unordered_map never moves its elements, so pointers stay valid
    node.hash = hash;
    node.header = header;
    node.parent = &parent;
    node.height = parent.height + 1;
    node.chain_work = parent.chain_work + block_proof(header.bits);
    parent.children.push_back(&node);
    if (node.chain_work > best_header_->chain_work) best_header_ = &node;
    put_index(batch, node);
    return node;
}

AcceptResult Chainstate::accept_header(const BlockHeader& header, std::int64_t now) {
    const AcceptResult result = check_header(header, now);
    if (result == AcceptResult::Accepted) {
        WriteBatch batch;
        add_header(header, batch);
        db_.write(batch);
    }
    return result;
}

BlockAcceptResult Chainstate::accept_block(const Block& block, std::int64_t now) {
    BlockIndex* node = nullptr;
    if (const auto found = tree_.find(block.hash()); found != tree_.end()) {
        node = &found->second;
        if (!node->invalid && node->have_data) return {AcceptResult::Duplicate, ConnectResult::Ok};
    } else if (const AcceptResult header = check_header(block.header, now); header != AcceptResult::Accepted) {
        return {header, ConnectResult::Ok};
    }

    if (node == nullptr || !node->invalid) {
        // A body that fails the context-free rules is turned away without marking the header: its
        // transactions may simply not be the ones the header commits to, and anyone can pair a
        // valid header with garbage.
        // The header's proof of work passed check_header just now, or when it entered the tree.
        if (check_block(block, params_, false) != BlockCheck::Ok) return {AcceptResult::Accepted, ConnectResult::BadBlock};
        WriteBatch batch;
        if (node == nullptr) node = &add_header(block.header, batch);
        node->pos = block_store_.append(block);  // on disk before the index entry that points at it
        node->have_data = true;
        put_index(batch, *node);
        link(*node, batch);
        db_.write(batch);
        activate_best_chain();
    }

    if (!node->invalid) return {};
    if (node->invalid_reason == ConnectResult::Ok) return {AcceptResult::InvalidParent, ConnectResult::Ok};
    return {AcceptResult::Accepted, node->invalid_reason};
}

// Called when `node`'s transactions arrive. If its parent is linked, it becomes linked too, and so
// does every descendant whose transactions were already waiting for it.
void Chainstate::link(BlockIndex& node, WriteBatch& batch) {
    if (node.parent != nullptr && !node.parent->linked) return;
    std::vector<BlockIndex*> queue{&node};
    for (std::size_t i = 0; i < queue.size(); ++i) {
        BlockIndex* n = queue[i];
        n->linked = true;
        n->sequence = next_sequence_++;
        if (!n->invalid) candidates_.insert(n);
        put_index(batch, *n);
        for (BlockIndex* child : n->children) {
            if (child->have_data) queue.push_back(child);
        }
    }
}

void Chainstate::mark_invalid(BlockIndex& node, ConnectResult reason, WriteBatch& batch) {
    node.invalid_reason = reason;
    std::vector<BlockIndex*> queue{&node};  // the block and everything built on it
    for (std::size_t i = 0; i < queue.size(); ++i) {
        queue[i]->invalid = true;
        candidates_.erase(queue[i]);
        put_index(batch, *queue[i]);
        for (BlockIndex* child : queue[i]->children) queue.push_back(child);
    }
    if (best_header_->invalid) find_best_header();
}

// Moves the tip to the best candidate: back to where the candidate's branch leaves the active
// chain, then forward along it. If a block on the way fails, it and its descendants drop out of
// the candidates and the loop picks again, which may lead straight back to the old chain.
void Chainstate::activate_best_chain() {
    chain_changed_ = false;
    while (true) {
        BlockIndex* best = *candidates_.begin();  // never empty: genesis is always a candidate
        if (best == active_.back()) break;

        BlockIndex* fork = best;
        while (!in_active_chain(*fork)) fork = fork->parent;
        while (active_.back() != fork) disconnect_tip();

        std::vector<BlockIndex*> path;
        for (BlockIndex* n = best; n != fork; n = n->parent) path.push_back(n);
        for (auto n = path.rbegin(); n != path.rend(); ++n) {
            if (!connect_tip(**n)) break;
        }
    }
    if (chain_changed_) {
        for (ChainListener* listener : listeners_) listener->tip_changed(*active_.back());
    }
}

void Chainstate::add_listener(ChainListener& listener) { listeners_.push_back(&listener); }

void Chainstate::remove_listener(ChainListener& listener) { std::erase(listeners_, &listener); }

// One write: the UTXO changes, the undo data and the new tip together.
bool Chainstate::connect_tip(BlockIndex& node) {
    const Block block = read_block(node);
    BlockUndo undo;
    const ConnectResult result = connect_block(block, node.height, utxos_, undo, params_);
    WriteBatch batch;
    if (result != ConnectResult::Ok) {
        mark_invalid(node, result, batch);
        db_.write(batch);
        return false;
    }

    // What connect_block just did in memory, in the same order: an output spent later in this
    // same block is put, then erased.
    for (const Transaction& tx : block.txs) {
        for (const OutPoint& in : tx.inputs) batch.erase(coin_key(in));
        const uint256 txid = tx.txid();
        for (std::uint32_t i = 0; i < tx.outputs.size(); ++i) {
            batch.put(coin_key({txid, i}), encode_coin(Coin{tx.outputs[i], node.height, tx.is_coinbase()}));
        }
    }
    batch.put(undo_key(node.hash), encode_undo(undo));
    batch.put(tip_key(), encode_hash(node.hash));
    db_.write(batch);
    active_.push_back(&node);
    chain_changed_ = true;
    for (ChainListener* listener : listeners_) listener->block_connected(block, node);
    return true;
}

// One write, the mirror image of connect_tip's.
void Chainstate::disconnect_tip() {
    BlockIndex& node = *active_.back();
    const Block block = read_block(node);
    const std::optional<Bytes> stored = db_.get(undo_key(node.hash));
    if (!stored) throw damaged(std::format("no undo data for #{}", node.height));
    const BlockUndo undo = decode_undo(*stored);
    disconnect_block(block, undo, utxos_);

    // What disconnect_block just did in memory, in the same order.
    WriteBatch batch;
    std::size_t next = undo.spent.size();
    for (auto tx = block.txs.rbegin(); tx != block.txs.rend(); ++tx) {
        const uint256 txid = tx->txid();
        for (std::uint32_t i = 0; i < tx->outputs.size(); ++i) batch.erase(coin_key({txid, i}));
        for (auto in = tx->inputs.rbegin(); in != tx->inputs.rend(); ++in) {
            batch.put(coin_key(*in), encode_coin(undo.spent[--next]));
        }
    }
    // The undo data stays (as Bitcoin's rev files do): reconnecting the block would write the same
    // bytes again, and an index that fell behind may still need them to take the block back out.
    batch.put(tip_key(), encode_hash(node.parent->hash));
    db_.write(batch);
    active_.pop_back();
    chain_changed_ = true;
    for (ChainListener* listener : listeners_) listener->block_disconnected(block, node);
}

const BlockIndex& Chainstate::at(int height) const { return *active_.at(static_cast<std::size_t>(height)); }

Block Chainstate::block_at(int height) const { return read_block(at(height)); }

bool Chainstate::in_active_chain(const BlockIndex& node) const {
    return node.height < static_cast<int>(active_.size()) && active_[static_cast<std::size_t>(node.height)] == &node;
}

const BlockIndex* Chainstate::find(const uint256& hash) const {
    const auto found = tree_.find(hash);
    return found == tree_.end() ? nullptr : &found->second;
}

}  // namespace bound
