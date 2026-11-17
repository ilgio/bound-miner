#pragma once

#include "core/amount.h"
#include "core/block.h"
#include "core/chain.h"
#include "core/mempool.h"
#include "core/storage.h"
#include "core/transaction.h"
#include "core/uint256.h"
#include "core/validation.h"
#include "crypto/ed25519.h"
#include "wallet/crypter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bound {

// Milestone 7: the wallet (Bitcoin Core's CWallet, much reduced).
//
// Keys. Every key comes from one random 32-byte seed:
//   key(branch, index) = KeyPair::from_seed(tagged_hash("BOUND/wallet-key", seed | branch | index))
// with branch 0 for addresses handed out and 1 for change, and the integers as u32 little-endian.
// Backing up the seed once covers every key the wallet will ever make. The seed is stored only
// encrypted under the passphrase (crypter.h), and spending needs it unlocked.
//
// The public keys of the next `lookahead` unused keys on each branch are derived ahead and stored
// in the clear (Bitcoin's keypool). That's what lets a locked wallet hand out addresses and
// recognise payments; the pool is topped up whenever the wallet is unlocked.
//
// Transactions. The wallet keeps every transaction that pays one of its keys or spends one of its
// coins, and follows the chain (as a ChainListener) and the mempool (as a MempoolListener). Which
// coins are spendable isn't stored: it's worked out from the chain's UTXO set and the mempool, so
// a reorganisation or a replaced transaction can never leave it stale.
//
// Not thread-safe: use it from the thread that owns the chainstate.

inline constexpr std::uint32_t kReceiveBranch = 0;
inline constexpr std::uint32_t kChangeBranch = 1;

KeyPair derive_key(const Seed& seed, std::uint32_t branch, std::uint32_t index);
Seed random_seed();  // from libsodium's CSPRNG

struct WalletError : std::runtime_error {
    enum class Kind {
        Locked,             // needs the passphrase: walletpassphrase first
        WrongPassphrase,
        InsufficientFunds,
        KeypoolEmpty,       // no unused keys left while locked
        NotFound,           // no such wallet transaction
        Invalid,            // the request can't be done: dust amount, a confirmed transaction to bump, ...
    };
    WalletError(Kind k, const std::string& what) : std::runtime_error(what), kind(k) {}
    Kind kind;
};

struct WalletOptions {
    std::uint32_t lookahead = 100;  // unused keys kept ready per branch
    KdfParams kdf;                  // for the passphrase of a new wallet, or a changed one
};

struct WalletTx {
    Transaction tx;
    uint256 txid;
    uint256 block;            // the block it's in (zero if none); see Wallet::status for whether it counts
    int height = -1;          // that block's height
    std::int64_t time = 0;    // when the wallet first saw it (for history from a scan: the block's time)
    std::uint64_t order = 0;  // the order the wallet learned of it
};

enum class TxStatus {
    Confirmed,  // in a block on the active chain
    InMempool,  // waiting for a block
    Inactive,   // neither: replaced, double-spent, dropped, or a coinbase whose block lost a reorg
};

struct WalletCoin {
    OutPoint op;
    TxOut out;
    int confirmations = 0;  // 0: in the mempool
    bool coinbase = false;
    bool change = false;     // paid to a change key
    bool spendable = false;  // mature, and confirmed or our own change
};

struct WalletBalance {
    Amount spendable = 0;    // confirmed and mature, plus our own unconfirmed change
    Amount unconfirmed = 0;  // incoming from others, still in the mempool
    Amount immature = 0;     // mined, waiting out coinbase maturity
};

struct WalletTxSummary {
    uint256 txid;
    Amount received = 0;  // paid to our keys
    Amount sent = 0;      // our coins it spends
    Amount fee = 0;       // when every input is ours; 0 otherwise
    bool from_me = false;
    bool coinbase = false;
    int confirmations = 0;  // -1: inactive
    std::int64_t time = 0;
};

class Wallet final : public ChainListener, public MempoolListener {
public:
    // A new wallet in `db`, which must hold none, from `seed` (random_seed() for a new one). To
    // restore a wallet from its seed, `rescan` the whole chain for its history; a new seed has
    // none. The wallet comes back locked. Throws std::runtime_error if db already has a wallet.
    // chain and mempool must outlive the wallet.
    static std::unique_ptr<Wallet> create(KeyValueStore& db, Chainstate& chain, Mempool& mempool,
                                          std::string_view passphrase, const Seed& seed, bool rescan,
                                          WalletOptions options = {});
    // The wallet in `db`, caught up with the chain (scanning the blocks it missed). Throws
    // std::runtime_error if db holds no wallet, or one for another network.
    static std::unique_ptr<Wallet> load(KeyValueStore& db, Chainstate& chain, Mempool& mempool, WalletOptions options = {});
    static bool exists(const KeyValueStore& db);

    ~Wallet() override;
    Wallet(const Wallet&) = delete;
    Wallet& operator=(const Wallet&) = delete;

    // The passphrase. Unlocking keeps the seed in memory until `until` (Unix time, checked by
    // tick) or lock(). Throws WalletError::WrongPassphrase.
    void unlock(std::string_view passphrase, std::int64_t until);
    void lock();
    bool locked() const { return !seed_.has_value(); }
    std::int64_t unlocked_until() const { return locked() ? 0 : unlocked_until_; }
    void tick(std::int64_t now);
    void change_passphrase(std::string_view old_passphrase, std::string_view new_passphrase);
    Seed seed() const;  // throws WalletError::Locked

    // The next unused receiving key. Throws WalletError::KeypoolEmpty if locked with none left.
    PublicKey new_address();
    bool is_mine(const PublicKey& key) const { return keys_.contains(key); }
    std::size_t keypool_size() const;  // unused keys ready, both branches

    WalletBalance balance() const;
    std::vector<WalletCoin> coins() const;  // unspent, oldest transaction first
    std::vector<WalletTxSummary> transactions() const;  // in the order the wallet learned of them
    const WalletTx* find(const uint256& txid) const;
    TxStatus status(const WalletTx& wtx) const;
    int confirmations(const WalletTx& wtx) const;  // 0 in the mempool, -1 inactive
    WalletTxSummary summarize(const WalletTx& wtx) const;
    std::size_t transaction_count() const { return txs_.size(); }

    // A signed payment of `amount` to `to`, at `rate` (at least the mempool's minimum relay fee),
    // with change back to a new change key unless it would be dust. Biggest coins first. Doesn't
    // submit it: hand it to the mempool (PeerManager::submit_transaction), and the wallet records
    // it when it gets in. Throws WalletError: Locked, Invalid (amount below the dust limit),
    // InsufficientFunds.
    Transaction create_payment(const PublicKey& to, Amount amount, FeeRate rate, ByteSpan memo = {});
    // Milestone 18 (the wallet app's Max): every spendable coin to `to` in one output, the fee taken
    // out of it; the biggest coins first if they don't all fit in one standard transaction. Throws
    // WalletError: Locked, Invalid (memo), InsufficientFunds (under the dust limit after the fee).
    Transaction create_sweep(const PublicKey& to, FeeRate rate, ByteSpan memo = {});
    // A replacement for our own `txid`, still in the mempool: the same inputs and payments, the
    // change cut (more coins added if needed) to pay `rate`, and enough to beat the original and
    // everything built on it, as full replace-by-fee requires. Throws WalletError.
    Transaction create_bump(const uint256& txid, FeeRate rate);
    // Our own transactions that are neither confirmed nor waiting, but could still be: their
    // inputs are unspent. The node offers them to the mempool again now and then.
    std::vector<Transaction> to_resubmit() const;

    void block_connected(const Block& block, const BlockIndex& node) override;
    void block_disconnected(const Block& block, const BlockIndex& node) override;
    void tx_added(const Transaction& tx, std::int64_t time) override;

private:
    struct KeyId {
        std::uint32_t branch = 0;
        std::uint32_t index = 0;
    };
    struct KeyHash {
        std::size_t operator()(const PublicKey& k) const noexcept;
    };

    Wallet(KeyValueStore& db, Chainstate& chain, Mempool& mempool, WalletOptions options);
    void sync();
    void write_meta(WriteBatch& batch) const;
    void top_up(WriteBatch& batch);
    void mark_used(const PublicKey& key, WriteBatch& batch);
    PublicKey take_key(std::uint32_t branch);
    bool relevant(const Transaction& tx) const;
    void store(const Transaction& tx, const BlockIndex* node, std::int64_t time, WriteBatch& batch);
    bool from_me(const Transaction& tx) const;
    Amount value_of(const OutPoint& op) const;  // precondition: mine_.contains(op)
    std::vector<WalletCoin> unspent(const std::set<uint256>& exclude) const;  // skipping outputs of `exclude`
    void scan_block(const Block& block, const BlockIndex& node, WriteBatch& batch);
    Transaction sign(Transaction tx) const;
    const Seed& unlocked_seed() const;

    KeyValueStore& db_;
    Chainstate& chain_;
    Mempool& mempool_;
    WalletOptions options_;
    Bytes encrypted_seed_;
    std::optional<Seed> seed_;  // while unlocked
    std::int64_t unlocked_until_ = 0;
    std::array<std::vector<PublicKey>, 2> pubkeys_;  // [branch][index], derived so far
    std::array<std::uint32_t, 2> next_{};            // [branch]: the first never handed out or seen used
    std::unordered_map<PublicKey, KeyId, KeyHash> keys_;
    std::map<uint256, WalletTx> txs_;          // by txid
    std::unordered_set<OutPoint> mine_;        // every output ever paid to our keys
    uint256 best_block_;                       // the block the wallet has seen the chain up to
    std::uint64_t next_order_ = 0;
};

const char* to_string(WalletError::Kind kind);

}  // namespace bound
