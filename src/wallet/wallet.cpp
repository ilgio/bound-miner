#include "wallet/wallet.h"

#include "crypto/sha256.h"
#include "util/serialize.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace bound {

namespace {

// What the wallet database holds. Keys:
//   'M'                          meta: version, network, encrypted seed, next key indices
//   'K' branch(u8) index(u32 BE) a derived public key (big-endian, so they load in order)
//   'T' txid                     a wallet transaction
//   'B'                          the block the wallet has seen the chain up to
constexpr std::uint32_t kWalletVersion = 1;
constexpr std::uint8_t kMetaKey = 'M';
constexpr std::uint8_t kKeyPrefix = 'K';
constexpr std::uint8_t kTxPrefix = 'T';
constexpr std::uint8_t kBestKey = 'B';

Bytes key_key(std::uint32_t branch, std::uint32_t index) {
    return Bytes{kKeyPrefix, static_cast<std::uint8_t>(branch), static_cast<std::uint8_t>(index >> 24),
                 static_cast<std::uint8_t>(index >> 16), static_cast<std::uint8_t>(index >> 8), static_cast<std::uint8_t>(index)};
}

Bytes tx_key(const uint256& txid) {
    Bytes key{kTxPrefix};
    key.insert(key.end(), txid.bytes().begin(), txid.bytes().end());
    return key;
}

Bytes hash_bytes(const uint256& h) { return Bytes(h.bytes().begin(), h.bytes().end()); }

uint256 read_hash(ByteReader& r) {
    const ByteSpan b = r.bytes(32);
    Digest32 d;
    std::copy(b.begin(), b.end(), d.begin());
    return uint256(d);
}

// Binds the encrypted seed to this kind of data and this network.
Bytes seed_ad(const ConsensusParams& params) {
    const ByteSpan tag = bytes_of("BOUND wallet seed");
    Bytes ad(tag.begin(), tag.end());
    ad.insert(ad.end(), params.genesis_hash.bytes().begin(), params.genesis_hash.bytes().end());
    return ad;
}

Bytes encode_wtx(const WalletTx& w) {
    ByteWriter out;
    out.var_bytes(w.tx.serialize());
    out.bytes(w.block.bytes());
    out.i32(w.height);
    out.i64(w.time);
    out.u64(w.order);
    return std::move(out).take();
}

WalletTx decode_wtx(ByteSpan data) {
    ByteReader r(data);
    WalletTx w;
    const Bytes tx = r.var_bytes(4 << 20);
    w.tx = Transaction::deserialize(tx);
    w.txid = w.tx.txid();
    w.block = read_hash(r);
    w.height = r.i32();
    w.time = r.i64();
    w.order = r.u64();
    r.expect_end();
    return w;
}

std::runtime_error damaged(const std::string& what) { return std::runtime_error("the wallet is damaged: " + what); }

}  // namespace

KeyPair derive_key(const Seed& seed, std::uint32_t branch, std::uint32_t index) {
    ByteWriter w;
    w.bytes(seed);
    w.u32(branch);
    w.u32(index);
    Bytes data = std::move(w).take();
    Digest32 key_seed = tagged_hash("BOUND/wallet-key", data);
    KeyPair key = KeyPair::from_seed(key_seed);
    sodium_memzero(data.data(), data.size());
    sodium_memzero(key_seed.data(), key_seed.size());
    return key;
}

const char* to_string(WalletError::Kind kind) {
    switch (kind) {
    case WalletError::Kind::Locked: return "locked";
    case WalletError::Kind::WrongPassphrase: return "wrong passphrase";
    case WalletError::Kind::InsufficientFunds: return "insufficient funds";
    case WalletError::Kind::KeypoolEmpty: return "keypool empty";
    case WalletError::Kind::NotFound: return "not found";
    case WalletError::Kind::Invalid: return "invalid";
    }
    return "?";
}

std::size_t Wallet::KeyHash::operator()(const PublicKey& k) const noexcept {
    std::size_t h = 0;
    std::memcpy(&h, k.data(), sizeof h);  // a public key's bytes are already uniformly spread
    return h;
}

Wallet::Wallet(KeyValueStore& db, Chainstate& chain, Mempool& mempool, WalletOptions options)
    : db_(db), chain_(chain), mempool_(mempool), options_(options) {}

Wallet::~Wallet() {
    chain_.remove_listener(*this);
    mempool_.remove_listener(*this);
    lock();
}

bool Wallet::exists(const KeyValueStore& db) { return db.get(Bytes{kMetaKey}).has_value(); }

Seed random_seed() {
    if (sodium_init() < 0) throw std::runtime_error("sodium_init failed");
    Seed seed;
    randombytes_buf(seed.data(), seed.size());
    return seed;
}

std::unique_ptr<Wallet> Wallet::create(KeyValueStore& db, Chainstate& chain, Mempool& mempool, std::string_view passphrase,
                                       const Seed& seed, bool rescan, WalletOptions options) {
    if (exists(db)) throw std::runtime_error("a wallet already exists here");
    std::unique_ptr<Wallet> w(new Wallet(db, chain, mempool, options));
    w->encrypted_seed_ = encrypt_secret(seed, passphrase, seed_ad(chain.params()), options.kdf);
    w->seed_ = seed;
    w->unlocked_until_ = 0;

    // A restored seed may have any history, so its scan starts at genesis.
    w->best_block_ = rescan ? chain.at(0).hash : chain.tip().hash;
    WriteBatch batch;
    w->top_up(batch);
    w->write_meta(batch);
    batch.put(Bytes{kBestKey}, hash_bytes(w->best_block_));
    db.write(batch);
    w->sync();  // with the seed at hand, so the keypool grows with whatever the scan finds
    w->lock();
    chain.add_listener(*w);
    mempool.add_listener(*w);
    return w;
}

std::unique_ptr<Wallet> Wallet::load(KeyValueStore& db, Chainstate& chain, Mempool& mempool, WalletOptions options) {
    const std::optional<Bytes> meta = db.get(Bytes{kMetaKey});
    if (!meta) throw std::runtime_error("there's no wallet here");
    std::unique_ptr<Wallet> w(new Wallet(db, chain, mempool, options));
    try {
        ByteReader r(*meta);
        const std::uint32_t version = r.u32();
        if (version != kWalletVersion) throw std::runtime_error("unknown wallet version " + std::to_string(version));
        if (read_hash(r) != chain.params().genesis_hash) throw std::runtime_error("the wallet belongs to another network");
        w->encrypted_seed_ = r.var_bytes(1 << 16);
        w->next_[0] = r.u32();
        w->next_[1] = r.u32();
        w->next_order_ = r.u64();
        r.expect_end();

        db.for_each(Bytes{kKeyPrefix}, [&](ByteSpan key, ByteSpan value) {
            if (key.size() != 6 || value.size() != 32 || key[1] > 1) throw damaged("a malformed key record");
            const std::uint32_t branch = key[1];
            const std::uint32_t index = std::uint32_t{key[2]} << 24 | std::uint32_t{key[3]} << 16 | std::uint32_t{key[4]} << 8 | key[5];
            if (index != w->pubkeys_[branch].size()) throw damaged("a gap in the keys");
            PublicKey pk;
            std::copy(value.begin(), value.end(), pk.begin());
            w->pubkeys_[branch].push_back(pk);
            w->keys_.emplace(pk, KeyId{branch, index});
        });
        db.for_each(Bytes{kTxPrefix}, [&](ByteSpan, ByteSpan value) {
            WalletTx wtx = decode_wtx(value);
            for (std::uint32_t i = 0; i < wtx.tx.outputs.size(); ++i) {
                if (w->keys_.contains(wtx.tx.outputs[i].recipient)) w->mine_.insert({wtx.txid, i});
            }
            w->txs_.emplace(wtx.txid, std::move(wtx));
        });
        const std::optional<Bytes> best = db.get(Bytes{kBestKey});
        if (!best || best->size() != 32) throw damaged("no best block");
        ByteReader best_reader(*best);
        w->best_block_ = read_hash(best_reader);
    } catch (const DeserializeError& e) {
        throw damaged(e.what());
    }
    w->sync();
    chain.add_listener(*w);
    mempool.add_listener(*w);
    return w;
}

// Catches up with the active chain from best_block_: whatever was confirmed in blocks that left
// the active chain meanwhile isn't any more, and the blocks after the fork are scanned. Then the
// mempool, for payments waiting there.
void Wallet::sync() {
    const BlockIndex* fork = chain_.find(best_block_);
    while (fork != nullptr && !chain_.in_active_chain(*fork)) fork = fork->parent;
    const int fork_height = fork != nullptr ? fork->height : -1;  // an unknown block: trust nothing

    WriteBatch batch;
    for (auto& [txid, wtx] : txs_) {
        if (wtx.height <= fork_height) continue;
        wtx.block = uint256{};
        wtx.height = -1;
        batch.put(tx_key(txid), encode_wtx(wtx));
    }
    for (int h = std::max(1, fork_height + 1); h <= chain_.height(); ++h) scan_block(chain_.block_at(h), chain_.at(h), batch);
    best_block_ = chain_.tip().hash;
    batch.put(Bytes{kBestKey}, hash_bytes(best_block_));
    for (const MempoolEntry* e : mempool_.entries()) {
        if (relevant(e->tx)) store(e->tx, nullptr, e->time, batch);
    }
    db_.write(batch);
}

void Wallet::write_meta(WriteBatch& batch) const {
    ByteWriter w;
    w.u32(kWalletVersion);
    w.bytes(chain_.params().genesis_hash.bytes());
    w.var_bytes(encrypted_seed_);
    w.u32(next_[0]);
    w.u32(next_[1]);
    w.u64(next_order_);
    batch.put(Bytes{kMetaKey}, std::move(w).take());
}

// Derives keys until each branch has `lookahead` unused ones. Needs the seed; locked, it does nothing.
void Wallet::top_up(WriteBatch& batch) {
    if (!seed_) return;
    for (std::uint32_t branch = 0; branch < 2; ++branch) {
        while (pubkeys_[branch].size() < std::size_t{next_[branch]} + options_.lookahead) {
            const auto index = static_cast<std::uint32_t>(pubkeys_[branch].size());
            const PublicKey pk = derive_key(*seed_, branch, index).public_key();
            pubkeys_[branch].push_back(pk);
            keys_.emplace(pk, KeyId{branch, index});
            batch.put(key_key(branch, index), Bytes(pk.begin(), pk.end()));
        }
    }
}

// A payment to `key` showed up: it and every key before it on its branch count as used.
void Wallet::mark_used(const PublicKey& key, WriteBatch& batch) {
    const KeyId id = keys_.at(key);
    if (id.index < next_[id.branch]) return;
    next_[id.branch] = id.index + 1;
    top_up(batch);
    write_meta(batch);
}

PublicKey Wallet::take_key(std::uint32_t branch) {
    WriteBatch batch;
    top_up(batch);
    if (next_[branch] >= pubkeys_[branch].size()) {
        throw WalletError(WalletError::Kind::KeypoolEmpty, "no unused keys left: unlock the wallet to make more");
    }
    const PublicKey pk = pubkeys_[branch][next_[branch]++];
    top_up(batch);
    write_meta(batch);
    db_.write(batch);
    return pk;
}

PublicKey Wallet::new_address() { return take_key(kReceiveBranch); }

std::size_t Wallet::keypool_size() const {
    return pubkeys_[0].size() - next_[0] + pubkeys_[1].size() - next_[1];
}

void Wallet::unlock(std::string_view passphrase, std::int64_t until) {
    std::optional<Bytes> secret = decrypt_secret(encrypted_seed_, passphrase, seed_ad(chain_.params()));
    if (!secret || secret->size() != Seed{}.size()) throw WalletError(WalletError::Kind::WrongPassphrase, "wrong passphrase");
    Seed s;
    std::copy(secret->begin(), secret->end(), s.begin());
    sodium_memzero(secret->data(), secret->size());
    seed_ = s;
    sodium_memzero(s.data(), s.size());
    unlocked_until_ = until;
    WriteBatch batch;
    top_up(batch);  // keys handed out while locked are replaced now
    if (!batch.empty()) {
        write_meta(batch);
        db_.write(batch);
    }
}

void Wallet::lock() {
    if (!seed_) return;
    sodium_memzero(seed_->data(), seed_->size());
    seed_.reset();
    unlocked_until_ = 0;
}

void Wallet::tick(std::int64_t now) {
    if (seed_ && now >= unlocked_until_) lock();
}

void Wallet::change_passphrase(std::string_view old_passphrase, std::string_view new_passphrase) {
    std::optional<Bytes> secret = decrypt_secret(encrypted_seed_, old_passphrase, seed_ad(chain_.params()));
    if (!secret) throw WalletError(WalletError::Kind::WrongPassphrase, "wrong passphrase");
    encrypted_seed_ = encrypt_secret(*secret, new_passphrase, seed_ad(chain_.params()), options_.kdf);
    sodium_memzero(secret->data(), secret->size());
    WriteBatch batch;
    write_meta(batch);
    db_.write(batch);
}

const Seed& Wallet::unlocked_seed() const {
    if (!seed_) throw WalletError(WalletError::Kind::Locked, "the wallet is locked: unlock it with walletpassphrase first");
    return *seed_;
}

Seed Wallet::seed() const { return unlocked_seed(); }

bool Wallet::relevant(const Transaction& tx) const {
    for (const TxOut& out : tx.outputs) {
        if (keys_.contains(out.recipient)) return true;
    }
    return std::any_of(tx.inputs.begin(), tx.inputs.end(), [&](const OutPoint& in) { return mine_.contains(in); });
}

bool Wallet::from_me(const Transaction& tx) const {
    return !tx.is_coinbase() && std::all_of(tx.inputs.begin(), tx.inputs.end(), [&](const OutPoint& in) { return mine_.contains(in); });
}

Amount Wallet::value_of(const OutPoint& op) const { return txs_.at(op.txid).tx.outputs.at(op.index).amount; }

// Records `tx`, confirmed in `node` or (node null) waiting in the mempool.
void Wallet::store(const Transaction& tx, const BlockIndex* node, std::int64_t time, WriteBatch& batch) {
    const uint256 txid = tx.txid();
    auto [it, inserted] = txs_.try_emplace(txid);
    WalletTx& w = it->second;
    if (inserted) {
        w.txid = txid;
        w.time = time;
        w.order = next_order_++;
    }
    w.tx = tx;  // a block's copy wins: its signatures are the ones that count
    if (node != nullptr) {
        w.block = node->hash;
        w.height = node->height;
    }
    for (std::uint32_t i = 0; i < tx.outputs.size(); ++i) {
        if (!keys_.contains(tx.outputs[i].recipient)) continue;
        mine_.insert({txid, i});
        mark_used(tx.outputs[i].recipient, batch);
    }
    batch.put(tx_key(txid), encode_wtx(w));
    write_meta(batch);
}

void Wallet::scan_block(const Block& block, const BlockIndex& node, WriteBatch& batch) {
    for (const Transaction& tx : block.txs) {
        if (relevant(tx)) store(tx, &node, node.header.time, batch);
    }
}

void Wallet::block_connected(const Block& block, const BlockIndex& node) {
    WriteBatch batch;
    scan_block(block, node, batch);
    best_block_ = node.hash;
    batch.put(Bytes{kBestKey}, hash_bytes(best_block_));
    db_.write(batch);
}

void Wallet::block_disconnected(const Block& block, const BlockIndex& node) {
    WriteBatch batch;
    for (const Transaction& tx : block.txs) {
        const auto found = txs_.find(tx.txid());
        if (found == txs_.end()) continue;
        found->second.block = uint256{};
        found->second.height = -1;
        batch.put(tx_key(found->first), encode_wtx(found->second));
    }
    best_block_ = node.parent->hash;
    batch.put(Bytes{kBestKey}, hash_bytes(best_block_));
    db_.write(batch);
}

void Wallet::tx_added(const Transaction& tx, std::int64_t time) {
    if (!relevant(tx)) return;
    WriteBatch batch;
    store(tx, nullptr, time, batch);
    db_.write(batch);
}

const WalletTx* Wallet::find(const uint256& txid) const {
    const auto found = txs_.find(txid);
    return found == txs_.end() ? nullptr : &found->second;
}

TxStatus Wallet::status(const WalletTx& wtx) const {
    if (!wtx.block.is_zero()) {
        const BlockIndex* node = chain_.find(wtx.block);
        if (node != nullptr && chain_.in_active_chain(*node)) return TxStatus::Confirmed;
    }
    if (mempool_.contains(wtx.txid)) return TxStatus::InMempool;
    return TxStatus::Inactive;
}

int Wallet::confirmations(const WalletTx& wtx) const {
    switch (status(wtx)) {
    case TxStatus::Confirmed: return chain_.height() - wtx.height + 1;
    case TxStatus::InMempool: return 0;
    case TxStatus::Inactive: return -1;
    }
    return -1;
}

std::vector<WalletCoin> Wallet::unspent(const std::set<uint256>& exclude) const {
    std::vector<const WalletTx*> ordered;
    for (const auto& [txid, w] : txs_) ordered.push_back(&w);
    std::sort(ordered.begin(), ordered.end(), [](const WalletTx* a, const WalletTx* b) { return a->order < b->order; });

    std::vector<WalletCoin> list;
    const int tip = chain_.height();
    for (const WalletTx* w : ordered) {
        if (exclude.contains(w->txid)) continue;
        const TxStatus st = status(*w);
        if (st == TxStatus::Inactive) continue;
        const bool coinbase = w->tx.is_coinbase();
        const bool trusted = st == TxStatus::Confirmed || from_me(w->tx);
        const bool mature = !coinbase || (st == TxStatus::Confirmed && tip + 1 - w->height >= chain_.params().coinbase_maturity);
        for (std::uint32_t i = 0; i < w->tx.outputs.size(); ++i) {
            const TxOut& out = w->tx.outputs[i];
            const auto key = keys_.find(out.recipient);
            if (key == keys_.end()) continue;
            const OutPoint op{w->txid, i};
            if (st == TxStatus::Confirmed && !chain_.utxos().contains(op)) continue;  // spent in the chain
            if (mempool_.spender(op) != nullptr) continue;                            // spent by a waiting transaction
            WalletCoin coin;
            coin.op = op;
            coin.out = out;
            coin.confirmations = st == TxStatus::Confirmed ? tip - w->height + 1 : 0;
            coin.coinbase = coinbase;
            coin.change = key->second.branch == kChangeBranch;
            coin.spendable = mature && trusted;
            list.push_back(coin);
        }
    }
    return list;
}

std::vector<WalletCoin> Wallet::coins() const { return unspent({}); }

WalletBalance Wallet::balance() const {
    WalletBalance b;
    for (const WalletCoin& coin : coins()) {
        if (coin.spendable) {
            b.spendable += coin.out.amount;
        } else if (coin.coinbase) {
            b.immature += coin.out.amount;
        } else {
            b.unconfirmed += coin.out.amount;
        }
    }
    return b;
}

WalletTxSummary Wallet::summarize(const WalletTx& wtx) const {
    WalletTxSummary s;
    s.txid = wtx.txid;
    s.coinbase = wtx.tx.is_coinbase();
    s.confirmations = confirmations(wtx);
    s.time = wtx.time;
    for (const TxOut& out : wtx.tx.outputs) {
        if (keys_.contains(out.recipient)) s.received += out.amount;
    }
    for (const OutPoint& in : wtx.tx.inputs) {
        if (mine_.contains(in)) s.sent += value_of(in);
    }
    s.from_me = from_me(wtx.tx);
    if (s.from_me) {
        Amount out_total = 0;
        for (const TxOut& out : wtx.tx.outputs) out_total += out.amount;
        s.fee = s.sent - out_total;
    }
    return s;
}

std::vector<WalletTxSummary> Wallet::transactions() const {
    std::vector<const WalletTx*> ordered;
    for (const auto& [txid, w] : txs_) ordered.push_back(&w);
    std::sort(ordered.begin(), ordered.end(), [](const WalletTx* a, const WalletTx* b) { return a->order < b->order; });
    std::vector<WalletTxSummary> list;
    for (const WalletTx* w : ordered) list.push_back(summarize(*w));
    return list;
}

Transaction Wallet::sign(Transaction tx) const {
    const Seed& seed = unlocked_seed();
    const uint256 sighash = tx.sighash();
    tx.signatures.clear();
    for (const OutPoint& in : tx.inputs) {
        const KeyId id = keys_.at(txs_.at(in.txid).tx.outputs.at(in.index).recipient);
        tx.signatures.push_back(derive_key(seed, id.branch, id.index).sign(sighash.bytes()));
    }
    return tx;
}

Transaction Wallet::create_payment(const PublicKey& to, Amount amount, FeeRate rate, ByteSpan memo) {
    unlocked_seed();
    const MempoolOptions& policy = mempool_.options();
    if (amount < policy.dust_limit || !money_range(amount)) {
        throw WalletError(WalletError::Kind::Invalid, "the amount must be at least the dust limit, " + format_coins(policy.dust_limit));
    }
    if (memo.size() > Transaction::kMaxMemo) throw WalletError(WalletError::Kind::Invalid, "the memo is over 100 bytes");
    rate = std::max(rate, policy.min_relay_fee);

    std::vector<WalletCoin> coins = this->coins();
    std::erase_if(coins, [](const WalletCoin& c) { return !c.spendable; });
    std::sort(coins.begin(), coins.end(), [](const WalletCoin& a, const WalletCoin& b) {
        if (a.out.amount != b.out.amount) return a.out.amount > b.out.amount;
        if (a.confirmations != b.confirmations) return a.confirmations > b.confirmations;
        return a.op < b.op;
    });

    // Biggest coins first, until they cover the amount and the fee with a change output.
    std::vector<OutPoint> inputs;
    Amount in_total = 0;
    for (const WalletCoin& coin : coins) {
        inputs.push_back(coin.op);
        in_total += coin.out.amount;
        if (in_total >= amount + rate.fee(transaction_size(inputs.size(), 2, memo.size()))) break;
    }
    const Amount fee_with_change = rate.fee(transaction_size(inputs.size(), 2, memo.size()));
    const Amount fee_alone = rate.fee(transaction_size(inputs.size(), 1, memo.size()));
    std::optional<Amount> change;
    if (in_total - amount - fee_with_change >= policy.dust_limit) {
        change = in_total - amount - fee_with_change;
    } else if (in_total - amount < fee_alone) {
        throw WalletError(WalletError::Kind::InsufficientFunds,
                          "insufficient funds: " + format_coins(in_total) + " spendable, " +
                              format_coins(amount + fee_alone) + " needed with the fee");
    }  // else: change would be dust, so it goes to the miner

    Transaction tx;
    tx.inputs = std::move(inputs);
    tx.outputs.push_back({amount, to});
    if (change) {
        tx.outputs.push_back({*change, take_key(kChangeBranch)});
        if (randombytes_uniform(2) == 1) std::swap(tx.outputs[0], tx.outputs[1]);  // don't give away which is change
    }
    tx.memo.assign(memo.begin(), memo.end());
    return sign(std::move(tx));
}

Transaction Wallet::create_sweep(const PublicKey& to, FeeRate rate, ByteSpan memo) {
    unlocked_seed();
    const MempoolOptions& policy = mempool_.options();
    if (memo.size() > Transaction::kMaxMemo) throw WalletError(WalletError::Kind::Invalid, "the memo is over 100 bytes");
    rate = std::max(rate, policy.min_relay_fee);

    std::vector<WalletCoin> coins = this->coins();
    std::erase_if(coins, [](const WalletCoin& c) { return !c.spendable; });
    std::sort(coins.begin(), coins.end(), [](const WalletCoin& a, const WalletCoin& b) {
        if (a.out.amount != b.out.amount) return a.out.amount > b.out.amount;
        return a.op < b.op;
    });
    Transaction tx;
    Amount total = 0;
    for (const WalletCoin& coin : coins) {
        if (transaction_size(tx.inputs.size() + 1, 1, memo.size()) > policy.max_tx_size) break;
        tx.inputs.push_back(coin.op);
        total += coin.out.amount;
    }
    const Amount fee = rate.fee(transaction_size(tx.inputs.size(), 1, memo.size()));
    if (tx.inputs.empty() || total - fee < policy.dust_limit) {
        throw WalletError(WalletError::Kind::InsufficientFunds,
                          "insufficient funds: " + format_coins(total) + " spendable, which doesn't cover the fee and the " +
                              format_coins(policy.dust_limit) + " an output needs");
    }
    tx.outputs.push_back({total - fee, to});
    tx.memo.assign(memo.begin(), memo.end());
    return sign(std::move(tx));
}

Transaction Wallet::create_bump(const uint256& txid, FeeRate rate) {
    unlocked_seed();
    const WalletTx* original = find(txid);
    if (original == nullptr) throw WalletError(WalletError::Kind::NotFound, "no such wallet transaction");
    const MempoolEntry* entry = mempool_.find(txid);
    if (entry == nullptr) throw WalletError(WalletError::Kind::Invalid, "only a transaction waiting in the mempool can be bumped");
    if (!from_me(original->tx)) throw WalletError(WalletError::Kind::Invalid, "only a transaction spending this wallet's coins can be bumped");
    const MempoolOptions& policy = mempool_.options();

    // The replacement evicts the original and everything built on it, and must pay for all of it.
    std::set<uint256> evicted;
    Amount evicted_fees = 0;
    for (const MempoolEntry* e : mempool_.with_descendants(txid)) {
        evicted.insert(e->txid);
        evicted_fees += e->fee;
    }
    rate = std::max({rate, policy.min_relay_fee, FeeRate{FeeRate::of(entry->fee, entry->size).per_kb + 1}});

    // Keep every payment; the change gets rebuilt to the same key.
    std::vector<TxOut> payments;
    std::optional<PublicKey> change_key;
    for (const TxOut& out : original->tx.outputs) {
        const auto key = keys_.find(out.recipient);
        if (!change_key && key != keys_.end() && key->second.branch == kChangeBranch) {
            change_key = out.recipient;
        } else {
            payments.push_back(out);
        }
    }
    Amount paying = 0;
    for (const TxOut& out : payments) paying += out.amount;

    std::vector<OutPoint> inputs = original->tx.inputs;
    Amount in_total = 0;
    for (const OutPoint& in : inputs) in_total += value_of(in);
    std::vector<WalletCoin> extra = unspent(evicted);
    std::erase_if(extra, [](const WalletCoin& c) { return !c.spendable; });
    std::sort(extra.begin(), extra.end(), [](const WalletCoin& a, const WalletCoin& b) { return a.out.amount > b.out.amount; });

    const std::size_t memo = original->tx.memo.size();
    const auto needed = [&](std::size_t outputs) {
        const std::size_t size = transaction_size(inputs.size(), outputs, memo);
        return std::max(rate.fee(size), evicted_fees + policy.min_relay_fee.fee(size));
    };
    std::optional<Amount> change;
    for (std::size_t next = 0;; ++next) {
        if (const Amount fee = needed(payments.size() + 1); in_total - paying - fee >= policy.dust_limit) {
            change = in_total - paying - fee;
            break;
        }
        if (!payments.empty() && in_total - paying >= needed(payments.size())) break;  // no change: the rest is fee
        if (next == extra.size()) {
            throw WalletError(WalletError::Kind::InsufficientFunds, "insufficient funds to pay the higher fee");
        }
        inputs.push_back(extra[next].op);
        in_total += extra[next].out.amount;
    }

    Transaction tx;
    tx.version = original->tx.version;
    tx.inputs = std::move(inputs);
    tx.outputs = std::move(payments);
    if (change) tx.outputs.push_back({*change, change_key ? *change_key : take_key(kChangeBranch)});
    tx.memo = original->tx.memo;
    return sign(std::move(tx));
}

std::vector<Transaction> Wallet::to_resubmit() const {
    std::vector<const WalletTx*> list;
    for (const auto& [txid, w] : txs_) {
        if (!from_me(w.tx) || status(w) != TxStatus::Inactive) continue;
        const bool open = std::all_of(w.tx.inputs.begin(), w.tx.inputs.end(), [&](const OutPoint& in) {
            const bool exists = chain_.utxos().contains(in) || mempool_.contains(in.txid);
            return exists && mempool_.spender(in) == nullptr;
        });
        if (open) list.push_back(&w);
    }
    std::sort(list.begin(), list.end(), [](const WalletTx* a, const WalletTx* b) { return a->order < b->order; });
    std::vector<Transaction> txs;
    for (const WalletTx* w : list) txs.push_back(w->tx);
    return txs;
}

}  // namespace bound
