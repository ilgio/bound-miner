#include "index/chainindex.h"

#include "util/serialize.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace bound {

namespace {

constexpr std::uint32_t kIndexVersion = 1;
constexpr std::uint8_t kMetaKey = 'M';
constexpr std::uint8_t kBestKey = 'B';
constexpr std::uint8_t kTxKey = 't';
constexpr std::uint8_t kSpentKey = 's';
constexpr std::uint8_t kDeltaKey = 'a';
constexpr std::uint8_t kCoinKey = 'u';
constexpr std::uint8_t kSummaryKey = 'y';
constexpr std::size_t kFlushOps = 50'000;  // while catching up, the changes gathered before a write

void put_be32(Bytes& key, std::uint32_t v) {
    key.push_back(static_cast<std::uint8_t>(v >> 24));
    key.push_back(static_cast<std::uint8_t>(v >> 16));
    key.push_back(static_cast<std::uint8_t>(v >> 8));
    key.push_back(static_cast<std::uint8_t>(v));
}

std::uint32_t be32(ByteSpan b) {
    return std::uint32_t{b[0]} << 24 | std::uint32_t{b[1]} << 16 | std::uint32_t{b[2]} << 8 | std::uint32_t{b[3]};
}

template <typename Range>
void append(Bytes& key, const Range& bytes) {
    key.insert(key.end(), bytes.begin(), bytes.end());
}

Bytes hash_bytes(const uint256& h) { return Bytes(h.bytes().begin(), h.bytes().end()); }

uint256 read_hash(ByteReader& r) {
    const ByteSpan b = r.bytes(32);
    Digest32 d;
    std::copy(b.begin(), b.end(), d.begin());
    return uint256(d);
}

Bytes tx_key(const uint256& txid) {
    Bytes key{kTxKey};
    append(key, txid.bytes());
    return key;
}

Bytes spent_key(const OutPoint& op) {
    Bytes key{kSpentKey};
    append(key, op.txid.bytes());
    put_be32(key, op.index);
    return key;
}

Bytes address_prefix(std::uint8_t kind, const PublicKey& address) {
    Bytes key{kind};
    append(key, address);
    return key;
}

Bytes delta_key(const PublicKey& address, int height, std::uint32_t position) {
    Bytes key = address_prefix(kDeltaKey, address);
    put_be32(key, static_cast<std::uint32_t>(height));
    put_be32(key, position);
    return key;
}

Bytes coin_key(const PublicKey& address, int height, const OutPoint& op) {
    Bytes key = address_prefix(kCoinKey, address);
    put_be32(key, static_cast<std::uint32_t>(height));
    append(key, op.txid.bytes());
    put_be32(key, op.index);
    return key;
}

Bytes encode_location(const uint256& block, std::uint32_t position) {
    ByteWriter w;
    w.bytes(block.bytes());
    w.u32(position);
    return std::move(w).take();
}

Bytes encode_delta(const uint256& txid, Amount received, Amount spent) {
    ByteWriter w;
    w.bytes(txid.bytes());
    w.i64(received);
    w.i64(spent);
    return std::move(w).take();
}

Bytes encode_coin(Amount amount, bool coinbase) {
    ByteWriter w;
    w.i64(amount);
    w.u8(coinbase ? 1 : 0);
    return std::move(w).take();
}

Bytes encode_summary(const AddressSummary& s) {
    ByteWriter w;
    w.i64(s.balance);
    w.i64(s.received);
    w.i64(s.sent);
    w.u64(s.txs);
    w.u64(s.coins);
    return std::move(w).take();
}

std::runtime_error damaged(const std::string& what) { return std::runtime_error("the index is damaged: " + what); }

// What one transaction does to each address it touches.
struct Effect {
    Amount received = 0;
    Amount spent = 0;
};

}  // namespace

ChainIndex::ChainIndex(KeyValueStore& db, Chainstate& chain, Logger log) : db_(db), chain_(chain), log_(std::move(log)) {
    try {
        if (const std::optional<Bytes> meta = db_.get(Bytes{kMetaKey})) {
            ByteReader r(*meta);
            const std::uint32_t version = r.u32();
            if (version != kIndexVersion) throw std::runtime_error(std::format("unknown index version {}", version));
            if (read_hash(r) != chain_.params().genesis_hash) throw std::runtime_error("the index belongs to another network");
            r.expect_end();
            const std::optional<Bytes> best = db_.get(Bytes{kBestKey});
            if (!best) throw damaged("no best block");
            ByteReader b(*best);
            best_ = read_hash(b);
            b.expect_end();
        } else {
            create();
        }
    } catch (const DeserializeError& e) {
        throw damaged(e.what());
    }
    sync();
    chain_.add_listener(*this);
}

ChainIndex::~ChainIndex() { chain_.remove_listener(*this); }

// An index of the genesis block alone: its transactions can be looked up, and nothing else.
void ChainIndex::create() {
    const BlockIndex& genesis = chain_.at(0);
    const Block block = chain_.read_block(genesis);
    WriteBatch batch;
    ByteWriter meta;
    meta.u32(kIndexVersion);
    meta.bytes(genesis.hash.bytes());
    batch.put(Bytes{kMetaKey}, std::move(meta).take());
    for (std::uint32_t pos = 0; pos < block.txs.size(); ++pos) {
        batch.put(tx_key(block.txs[pos].txid()), encode_location(genesis.hash, pos));
    }
    batch.put(Bytes{kBestKey}, hash_bytes(genesis.hash));
    db_.write(batch);
    best_ = genesis.hash;
}

void ChainIndex::wipe() {
    while (true) {
        WriteBatch batch;
        db_.scan({}, false, [&](ByteSpan key, ByteSpan) {
            batch.erase(Bytes(key.begin(), key.end()));
            return batch.ops().size() < 10'000;
        });
        if (batch.empty()) return;
        db_.write(batch);
    }
}

// From wherever the index stopped to the chain's tip: back out of the blocks that aren't on the
// active chain any more, then read the ones it hasn't seen.
void ChainIndex::sync() {
    const BlockIndex* at = chain_.find(best_);
    if (at == nullptr) {
        // It describes blocks this node has never heard of: the chain data was replaced under it.
        if (log_) log_("index: it doesn't match this chain; building it again");
        wipe();
        create();
        at = &chain_.at(0);
    }
    Pending pending;
    while (!chain_.in_active_chain(*at)) {
        revert(chain_.read_block(*at), *at, pending);
        at = at->parent;
        best_ = at->hash;
        if (pending.batch.ops().size() >= kFlushOps) flush(pending);
    }
    const int from = at->height;
    for (int h = from + 1; h <= chain_.height(); ++h) {
        const BlockIndex& node = chain_.at(h);
        apply(chain_.read_block(node), node, pending);
        best_ = node.hash;
        if (pending.batch.ops().size() >= kFlushOps) flush(pending);
        if (log_ && (h - from) % 10'000 == 0) log_(std::format("index: at block #{} of {}", h, chain_.height()));
    }
    flush(pending);
    if (log_ && chain_.height() > from) {
        log_(std::format("index: read {} blocks, now in step at #{}", chain_.height() - from, chain_.height()));
    }
}

BlockUndo ChainIndex::undo_of(const BlockIndex& node) const {
    std::optional<BlockUndo> undo = chain_.read_undo(node);
    if (!undo) throw damaged(std::format("the chain has no undo data for block #{}", node.height));
    return std::move(*undo);
}

AddressSummary& ChainIndex::summary_in(Pending& pending, const PublicKey& address) const {
    const auto [it, inserted] = pending.summaries.try_emplace(address);
    if (inserted) it->second = summary(address);
    return it->second;
}

void ChainIndex::apply(const Block& block, const BlockIndex& node, Pending& pending) const {
    const BlockUndo undo = undo_of(node);
    std::size_t next_spent = 0;
    for (std::uint32_t pos = 0; pos < block.txs.size(); ++pos) {
        const Transaction& tx = block.txs[pos];
        const uint256 txid = tx.txid();
        pending.batch.put(tx_key(txid), encode_location(node.hash, pos));

        std::map<PublicKey, Effect> effects;
        for (const OutPoint& in : tx.inputs) {
            if (next_spent >= undo.spent.size()) throw damaged(std::format("undo data too short for block #{}", node.height));
            const Coin& coin = undo.spent[next_spent++];
            effects[coin.out.recipient].spent += coin.out.amount;
            pending.batch.erase(coin_key(coin.out.recipient, coin.height, in));
            pending.batch.put(spent_key(in), hash_bytes(txid));
            AddressSummary& s = summary_in(pending, coin.out.recipient);
            s.balance -= coin.out.amount;
            s.sent += coin.out.amount;
            s.coins -= 1;
        }
        for (std::uint32_t n = 0; n < tx.outputs.size(); ++n) {
            const TxOut& out = tx.outputs[n];
            effects[out.recipient].received += out.amount;
            pending.batch.put(coin_key(out.recipient, node.height, {txid, n}), encode_coin(out.amount, tx.is_coinbase()));
            AddressSummary& s = summary_in(pending, out.recipient);
            s.balance += out.amount;
            s.received += out.amount;
            s.coins += 1;
        }
        for (const auto& [address, effect] : effects) {
            pending.batch.put(delta_key(address, node.height, pos), encode_delta(txid, effect.received, effect.spent));
            summary_in(pending, address).txs += 1;
        }
    }
    if (next_spent != undo.spent.size()) throw damaged(std::format("undo data too long for block #{}", node.height));
}

// apply() backwards: the last transaction first, and within it the outputs before the inputs, so
// an output made and spent in the same block comes out in the right order.
void ChainIndex::revert(const Block& block, const BlockIndex& node, Pending& pending) const {
    const BlockUndo undo = undo_of(node);
    std::size_t next_spent = undo.spent.size();
    for (std::uint32_t pos = static_cast<std::uint32_t>(block.txs.size()); pos-- > 0;) {
        const Transaction& tx = block.txs[pos];
        const uint256 txid = tx.txid();

        std::map<PublicKey, Effect> effects;
        for (std::uint32_t n = static_cast<std::uint32_t>(tx.outputs.size()); n-- > 0;) {
            const TxOut& out = tx.outputs[n];
            effects[out.recipient];
            pending.batch.erase(coin_key(out.recipient, node.height, {txid, n}));
            AddressSummary& s = summary_in(pending, out.recipient);
            s.balance -= out.amount;
            s.received -= out.amount;
            s.coins -= 1;
        }
        for (auto in = tx.inputs.rbegin(); in != tx.inputs.rend(); ++in) {
            if (next_spent == 0) throw damaged(std::format("undo data too short for block #{}", node.height));
            const Coin& coin = undo.spent[--next_spent];
            effects[coin.out.recipient];
            pending.batch.put(coin_key(coin.out.recipient, coin.height, *in), encode_coin(coin.out.amount, coin.coinbase));
            pending.batch.erase(spent_key(*in));
            AddressSummary& s = summary_in(pending, coin.out.recipient);
            s.balance += coin.out.amount;
            s.sent -= coin.out.amount;
            s.coins += 1;
        }
        for (const auto& [address, effect] : effects) {
            pending.batch.erase(delta_key(address, node.height, pos));
            summary_in(pending, address).txs -= 1;
        }
        pending.batch.erase(tx_key(txid));
    }
    if (next_spent != 0) throw damaged(std::format("undo data too long for block #{}", node.height));
}

void ChainIndex::flush(Pending& pending) {
    for (const auto& [address, s] : pending.summaries) {
        if (s == AddressSummary{}) {
            pending.batch.erase(address_prefix(kSummaryKey, address));
        } else {
            pending.batch.put(address_prefix(kSummaryKey, address), encode_summary(s));
        }
    }
    pending.batch.put(Bytes{kBestKey}, hash_bytes(best_));
    db_.write(pending.batch);
    pending = Pending{};
}

void ChainIndex::block_connected(const Block& block, const BlockIndex& node) {
    if (node.parent == nullptr || node.parent->hash != best_) throw std::logic_error("the index is out of step with the chain");
    Pending pending;
    apply(block, node, pending);
    best_ = node.hash;
    flush(pending);
}

void ChainIndex::block_disconnected(const Block& block, const BlockIndex& node) {
    if (node.hash != best_ || node.parent == nullptr) throw std::logic_error("the index is out of step with the chain");
    Pending pending;
    revert(block, node, pending);
    best_ = node.parent->hash;
    flush(pending);
}

std::optional<TxLocation> ChainIndex::find_tx(const uint256& txid) const {
    const std::optional<Bytes> value = db_.get(tx_key(txid));
    if (!value) return std::nullopt;
    ByteReader r(*value);
    TxLocation location;
    location.block = read_hash(r);
    location.position = r.u32();
    r.expect_end();
    return location;
}

std::optional<ChainIndex::FoundTx> ChainIndex::read_tx(const uint256& txid) const {
    const std::optional<TxLocation> location = find_tx(txid);
    if (!location) return std::nullopt;
    const BlockIndex* node = chain_.find(location->block);
    if (node == nullptr || !chain_.in_active_chain(*node)) return std::nullopt;
    Block block = chain_.read_block(*node);
    if (location->position >= block.txs.size()) throw damaged("a transaction's position is past its block's end");
    return FoundTx{std::move(block.txs[location->position]), node, location->position};
}

std::optional<uint256> ChainIndex::spender(const OutPoint& op) const {
    const std::optional<Bytes> value = db_.get(spent_key(op));
    if (!value) return std::nullopt;
    ByteReader r(*value);
    const uint256 txid = read_hash(r);
    r.expect_end();
    return txid;
}

AddressSummary ChainIndex::summary(const PublicKey& address) const {
    const std::optional<Bytes> value = db_.get(address_prefix(kSummaryKey, address));
    if (!value) return {};
    ByteReader r(*value);
    AddressSummary s;
    s.balance = r.i64();
    s.received = r.i64();
    s.sent = r.i64();
    s.txs = r.u64();
    s.coins = r.u64();
    r.expect_end();
    return s;
}

std::vector<AddressDelta> ChainIndex::history(const PublicKey& address, std::size_t skip, std::size_t count) const {
    std::vector<AddressDelta> list;
    if (count == 0) return list;
    const Bytes prefix = address_prefix(kDeltaKey, address);
    db_.scan(prefix, true, [&](ByteSpan key, ByteSpan value) {
        if (skip > 0) {
            --skip;
            return true;
        }
        if (key.size() != prefix.size() + 8) throw damaged("a malformed history key");
        AddressDelta d;
        d.height = static_cast<int>(be32(key.subspan(prefix.size(), 4)));
        d.position = be32(key.subspan(prefix.size() + 4, 4));
        ByteReader r(value);
        d.txid = read_hash(r);
        d.received = r.i64();
        d.spent = r.i64();
        r.expect_end();
        list.push_back(d);
        return list.size() < count;
    });
    return list;
}

std::vector<AddressCoin> ChainIndex::coins(const PublicKey& address, std::size_t skip, std::size_t count) const {
    std::vector<AddressCoin> list;
    if (count == 0) return list;
    const Bytes prefix = address_prefix(kCoinKey, address);
    db_.scan(prefix, true, [&](ByteSpan key, ByteSpan value) {
        if (skip > 0) {
            --skip;
            return true;
        }
        if (key.size() != prefix.size() + 40) throw damaged("a malformed coin key");
        AddressCoin c;
        c.height = static_cast<int>(be32(key.subspan(prefix.size(), 4)));
        ByteReader k(key.subspan(prefix.size() + 4, 32));
        c.op.txid = read_hash(k);
        c.op.index = be32(key.subspan(prefix.size() + 36, 4));
        ByteReader r(value);
        c.amount = r.i64();
        c.coinbase = r.u8() != 0;
        r.expect_end();
        list.push_back(c);
        return list.size() < count;
    });
    return list;
}

}  // namespace bound
