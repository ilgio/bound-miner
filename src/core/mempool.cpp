#include "core/mempool.h"

#include "core/consensus.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace bound {

namespace {

// 64 x 64 -> 128-bit product, for comparing fee rates exactly.
struct U128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    friend auto operator<=>(const U128&, const U128&) = default;
};

U128 multiply(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a_lo = a & 0xffffffff, a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xffffffff, b_hi = b >> 32;
    const std::uint64_t ll = a_lo * b_lo;
    const std::uint64_t lh = a_lo * b_hi;
    const std::uint64_t hl = a_hi * b_lo;
    const std::uint64_t hh = a_hi * b_hi;
    const std::uint64_t mid = (ll >> 32) + (lh & 0xffffffff) + (hl & 0xffffffff);
    return {hh + (lh >> 32) + (hl >> 32) + (mid >> 32), (mid << 32) | (ll & 0xffffffff)};
}

std::size_t compact_size_length(std::size_t n) { return n < 0xfd ? 1 : n <= 0xffff ? 3 : n <= 0xffffffff ? 5 : 9; }

const char* describe(TxCheck c) {
    switch (c) {
    case TxCheck::Ok: return "ok";
    case TxCheck::NoOutputs: return "no outputs";
    case TxCheck::OutputOutOfRange: return "an output amount out of range";
    case TxCheck::TotalOutOfRange: return "the output total out of range";
    case TxCheck::DuplicateInput: return "spends the same output twice";
    case TxCheck::MemoTooLong: return "memo too long";
    case TxCheck::SignatureCount: return "wrong number of signatures";
    case TxCheck::BadLockTime: return "non-zero lock time";
    }
    return "?";
}

const char* describe(InputsCheck c) {
    switch (c) {
    case InputsCheck::Ok: return "ok";
    case InputsCheck::MissingInput: return "missing inputs";
    case InputsCheck::ImmatureCoinbase: return "spends an immature coinbase output";
    case InputsCheck::InputValueOutOfRange: return "an input value out of range";
    case InputsCheck::InputsBelowOutputs: return "outputs exceed inputs";
    case InputsCheck::BadSignature: return "bad signature";
    }
    return "?";
}

MempoolAccept reject(MempoolResult r, std::string reason) {
    MempoolAccept a;
    a.result = r;
    a.reason = std::move(reason);
    return a;
}

}  // namespace

FeeRate FeeRate::of(Amount fee, std::size_t bytes) {
    return {bytes == 0 ? 0 : fee * 1000 / static_cast<Amount>(bytes)};
}

Amount FeeRate::fee(std::size_t bytes) const { return (per_kb * static_cast<Amount>(bytes) + 999) / 1000; }

std::strong_ordering compare_rates(Amount fee_a, std::size_t size_a, Amount fee_b, std::size_t size_b) {
    return multiply(static_cast<std::uint64_t>(fee_a), size_b) <=> multiply(static_cast<std::uint64_t>(fee_b), size_a);
}

std::size_t transaction_size(std::size_t inputs, std::size_t outputs, std::size_t memo_size) {
    return 4 + compact_size_length(inputs) + 36 * inputs + compact_size_length(outputs) + 40 * outputs + 4 +
           compact_size_length(memo_size) + memo_size + 64 * inputs;
}

const char* to_string(MempoolResult r) {
    switch (r) {
    case MempoolResult::Accepted: return "accepted";
    case MempoolResult::Duplicate: return "duplicate";
    case MempoolResult::Invalid: return "invalid";
    case MempoolResult::NonStandard: return "non-standard";
    case MempoolResult::MissingInputs: return "missing inputs";
    case MempoolResult::Premature: return "premature";
    case MempoolResult::FeeTooLow: return "fee too low";
    case MempoolResult::Conflict: return "conflict";
    case MempoolResult::TooLongChain: return "too long chain";
    case MempoolResult::MempoolFull: return "mempool full";
    }
    return "?";
}

Mempool::Mempool(Chainstate& chain, MempoolOptions options) : chain_(chain), options_(options) {
    chain_.add_listener(*this);
}

Mempool::~Mempool() { chain_.remove_listener(*this); }

MempoolAccept Mempool::accept(const Transaction& tx, std::int64_t now) { return add(tx, now, true); }

MempoolAccept Mempool::add(const Transaction& tx, std::int64_t time, bool notify) {
    const uint256 txid = tx.txid();
    if (entries_.contains(txid)) return reject(MempoolResult::Duplicate, "already in the mempool");
    if (tx.is_coinbase()) return reject(MempoolResult::Invalid, "a coinbase is only valid in a block");
    if (const TxCheck check = check_transaction(tx); check != TxCheck::Ok) return reject(MempoolResult::Invalid, describe(check));

    // Policy: what this node passes on, beyond what's valid.
    if (tx.version != 1) return reject(MempoolResult::NonStandard, "unknown transaction version");
    const std::size_t size = tx.serialize().size();
    if (size > options_.max_tx_size) return reject(MempoolResult::NonStandard, "too large");
    for (const TxOut& out : tx.outputs) {
        if (out.amount < options_.dust_limit) return reject(MempoolResult::NonStandard, "an output below the dust limit");
    }

    // The waiting transactions it double-spends, and everything built on them: what it would evict.
    std::set<uint256> conflicts;
    for (const OutPoint& in : tx.inputs) {
        if (const auto found = spenders_.find(in); found != spenders_.end()) conflicts.insert(found->second);
    }
    const std::set<uint256> evict = descendants_of(conflicts);
    if (evict.size() > options_.max_replaced) return reject(MempoolResult::Conflict, "would replace too many transactions");

    // The coins it spends: unspent in the chain, or outputs of waiting transactions that stay.
    const int spend_height = chain_.height() + 1;
    UtxoSet coins;
    std::set<uint256> parents;
    for (const OutPoint& in : tx.inputs) {
        if (const Coin* coin = chain_.utxos().find(in)) {
            coins.add(in, *coin);
            continue;
        }
        const auto parent = entries_.find(in.txid);
        if (parent == entries_.end() || in.index >= parent->second.tx.outputs.size()) {
            return reject(MempoolResult::MissingInputs, "spends an unknown or already spent output");
        }
        if (evict.contains(in.txid)) return reject(MempoolResult::Conflict, "spends an output of a transaction it would replace");
        coins.add(in, Coin{parent->second.tx.outputs[in.index], spend_height, false});
        parents.insert(in.txid);
    }

    // Maturity and the fee, before any signature is checked.
    Amount in_total = 0;
    for (const OutPoint& in : tx.inputs) {
        const Coin& coin = *coins.find(in);
        if (coin.coinbase && spend_height - coin.height < chain_.params().coinbase_maturity) {
            return reject(MempoolResult::Premature, "spends an immature coinbase output");
        }
        in_total += coin.out.amount;  // at most 2,778 inputs fit in max_tx_size: no overflow
    }
    Amount out_total = 0;
    for (const TxOut& out : tx.outputs) out_total += out.amount;  // bounded by check_transaction
    if (out_total > in_total) return reject(MempoolResult::Invalid, describe(InputsCheck::InputsBelowOutputs));
    const Amount fee = in_total - out_total;
    if (fee < options_.min_relay_fee.fee(size)) return reject(MempoolResult::FeeTooLow, "fee below the minimum relay fee rate");

    // Full replace-by-fee: a better rate than each transaction it double-spends, and enough fee to
    // pay for everything it evicts plus its own relay.
    if (!conflicts.empty()) {
        for (const uint256& id : conflicts) {
            const MempoolEntry& old = entries_.at(id);
            if (compare_rates(fee, size, old.fee, old.size) != std::strong_ordering::greater) {
                return reject(MempoolResult::Conflict, "double spend without a higher fee rate");
            }
        }
        Amount evicted_fees = 0;
        for (const uint256& id : evict) evicted_fees += entries_.at(id).fee;
        if (fee < evicted_fees + options_.min_relay_fee.fee(size)) {
            return reject(MempoolResult::Conflict, "double spend that doesn't pay for what it replaces");
        }
    }

    // Unconfirmed chains stay short: long ones are expensive to track, select and evict.
    const std::set<uint256> ancestors = ancestors_of(parents);
    if (ancestors.size() + 1 > options_.ancestor_limit) return reject(MempoolResult::TooLongChain, "too many unconfirmed ancestors");
    for (const uint256& id : ancestors) {
        std::size_t descendants = 0;
        for (const uint256& d : descendants_of({id})) descendants += evict.contains(d) ? 0 : 1;
        if (descendants + 1 > options_.descendant_limit) {
            return reject(MempoolResult::TooLongChain, "an ancestor has too many unconfirmed descendants");
        }
    }

    // Last, the expensive part: every signature (and the other input rules once more).
    if (const InputsCheck check = check_tx_inputs(tx, coins, spend_height, chain_.params(), nullptr); check != InputsCheck::Ok) {
        return reject(MempoolResult::Invalid, describe(check));
    }

    MempoolAccept result;
    result.fee = fee;
    result.replaced.assign(evict.begin(), evict.end());
    for (const uint256& id : evict) remove_one(id);

    MempoolEntry& entry = entries_[txid];  // references into an unordered_map survive rehashing
    entry.tx = tx;
    entry.txid = txid;
    entry.wtxid = tx.wtxid();
    entry.fee = fee;
    entry.size = size;
    entry.time = time;
    entry.sequence = next_sequence_++;
    entry.parents = parents;
    for (const uint256& p : parents) entries_.at(p).children.insert(txid);
    for (const OutPoint& in : tx.inputs) spenders_[in] = txid;
    by_wtxid_[entry.wtxid] = txid;
    bytes_ += size;
    fees_ += fee;

    if (bytes_ > options_.max_bytes) {
        trim();
        if (!entries_.contains(txid)) return reject(MempoolResult::MempoolFull, "mempool full, and the fee rate is too low");
    }
    if (notify) {
        for (MempoolListener* listener : listeners_) listener->tx_added(tx, time);
    }
    return result;
}

std::set<uint256> Mempool::descendants_of(const std::set<uint256>& roots) const {
    std::set<uint256> found = roots;
    std::vector<uint256> queue(roots.begin(), roots.end());
    while (!queue.empty()) {
        const uint256 id = queue.back();
        queue.pop_back();
        for (const uint256& child : entries_.at(id).children) {
            if (found.insert(child).second) queue.push_back(child);
        }
    }
    return found;
}

std::set<uint256> Mempool::ancestors_of(const std::set<uint256>& parents) const {
    std::set<uint256> found = parents;
    std::vector<uint256> queue(parents.begin(), parents.end());
    while (!queue.empty()) {
        const uint256 id = queue.back();
        queue.pop_back();
        for (const uint256& parent : entries_.at(id).parents) {
            if (found.insert(parent).second) queue.push_back(parent);
        }
    }
    return found;
}

void Mempool::remove_one(uint256 txid) {
    const auto found = entries_.find(txid);
    if (found == entries_.end()) return;
    const MempoolEntry& e = found->second;
    for (const uint256& p : e.parents) {
        if (const auto parent = entries_.find(p); parent != entries_.end()) parent->second.children.erase(txid);
    }
    for (const uint256& c : e.children) {
        if (const auto child = entries_.find(c); child != entries_.end()) child->second.parents.erase(txid);
    }
    for (const OutPoint& in : e.tx.inputs) spenders_.erase(in);
    by_wtxid_.erase(e.wtxid);
    bytes_ -= e.size;
    fees_ -= e.fee;
    entries_.erase(found);
}

void Mempool::remove_with_descendants(uint256 txid) {
    if (!entries_.contains(txid)) return;
    for (const uint256& id : descendants_of({txid})) remove_one(id);
}

// Evicts by descendant score, as Bitcoin's TrimToSize does: the transaction whose package (it
// plus its descendants) pays the lowest rate goes first, with its descendants, until the mempool
// fits. A package's score is the better of its own rate and the package's, so a high-fee
// transaction isn't evicted for its cheap children. Ties evict the newest.
void Mempool::trim() {
    while (bytes_ > options_.max_bytes && !entries_.empty()) {
        const MempoolEntry* worst = nullptr;
        Amount worst_fee = 0;
        std::size_t worst_size = 1;
        for (const auto& [id, e] : entries_) {
            Amount fee = 0;
            std::size_t size = 0;
            for (const uint256& d : descendants_of({id})) {
                fee += entries_.at(d).fee;
                size += entries_.at(d).size;
            }
            if (compare_rates(e.fee, e.size, fee, size) == std::strong_ordering::greater) {
                fee = e.fee;
                size = e.size;
            }
            const auto c = worst == nullptr ? std::strong_ordering::less : compare_rates(fee, size, worst_fee, worst_size);
            if (c == std::strong_ordering::less || (c == std::strong_ordering::equal && e.sequence > worst->sequence)) {
                worst = &e;
                worst_fee = fee;
                worst_size = size;
            }
        }
        remove_with_descendants(worst->txid);
    }
}

const MempoolEntry* Mempool::find(const uint256& txid) const {
    const auto found = entries_.find(txid);
    return found == entries_.end() ? nullptr : &found->second;
}

const MempoolEntry* Mempool::find_wtxid(const uint256& wtxid) const {
    const auto found = by_wtxid_.find(wtxid);
    return found == by_wtxid_.end() ? nullptr : find(found->second);
}

const MempoolEntry* Mempool::spender(const OutPoint& op) const {
    const auto found = spenders_.find(op);
    return found == spenders_.end() ? nullptr : find(found->second);
}

std::vector<const MempoolEntry*> Mempool::with_descendants(const uint256& txid) const {
    std::vector<const MempoolEntry*> list;
    if (!entries_.contains(txid)) return list;
    for (const uint256& id : descendants_of({txid})) list.push_back(&entries_.at(id));
    std::sort(list.begin(), list.end(), [](const MempoolEntry* a, const MempoolEntry* b) { return a->sequence < b->sequence; });
    return list;
}

std::vector<const MempoolEntry*> Mempool::entries() const {
    std::vector<const MempoolEntry*> list;
    list.reserve(entries_.size());
    for (const auto& [id, e] : entries_) list.push_back(&e);
    std::sort(list.begin(), list.end(), [](const MempoolEntry* a, const MempoolEntry* b) { return a->sequence < b->sequence; });
    return list;
}

Mempool::Selection Mempool::select(std::size_t max_bytes) const {
    // Each candidate's package: it plus its ancestors not taken yet.
    struct Candidate {
        Amount fee = 0;
        std::size_t size = 0;
        std::uint64_t sequence = 0;
        uint256 txid;
    };
    struct Better {
        bool operator()(const Candidate& a, const Candidate& b) const {
            const std::strong_ordering c = compare_rates(a.fee, a.size, b.fee, b.size);
            if (c != std::strong_ordering::equal) return c == std::strong_ordering::greater;
            return a.sequence < b.sequence;  // equal rates: first come, first served
        }
    };

    std::unordered_map<uint256, std::set<uint256>> ancestors;
    for (const auto& [id, e] : entries_) ancestors.emplace(id, ancestors_of(e.parents));
    std::unordered_set<uint256> taken;
    const auto package = [&](const uint256& id) {
        const MempoolEntry& e = entries_.at(id);
        Candidate c{e.fee, e.size, e.sequence, id};
        for (const uint256& a : ancestors.at(id)) {
            if (taken.contains(a)) continue;
            c.fee += entries_.at(a).fee;
            c.size += entries_.at(a).size;
        }
        return c;
    };

    std::set<Candidate, Better> queue;
    std::unordered_map<uint256, Candidate> queued;
    for (const auto& [id, e] : entries_) {
        const Candidate c = package(id);
        queue.insert(c);
        queued.emplace(id, c);
    }

    Selection selection;
    while (!queue.empty()) {
        const Candidate best = *queue.begin();
        queue.erase(queue.begin());
        queued.erase(best.txid);
        if (selection.bytes + best.size > max_bytes) {
            selection.all = false;  // doesn't fit; something smaller still might
            continue;
        }

        std::vector<const MempoolEntry*> members{&entries_.at(best.txid)};
        for (const uint256& a : ancestors.at(best.txid)) {
            if (!taken.contains(a)) members.push_back(&entries_.at(a));
        }
        std::sort(members.begin(), members.end(), [](const MempoolEntry* a, const MempoolEntry* b) { return a->sequence < b->sequence; });
        const bool first = selection.txs.empty();
        std::set<uint256> roots;
        for (const MempoolEntry* m : members) {
            taken.insert(m->txid);
            roots.insert(m->txid);
            selection.txs.push_back(m->tx);
            if (const auto q = queued.find(m->txid); q != queued.end()) {
                queue.erase(q->second);
                queued.erase(q);
            }
        }
        selection.bytes += best.size;
        selection.fees += best.fee;
        const FeeRate rate = FeeRate::of(best.fee, best.size);
        if (first || rate < selection.lowest) selection.lowest = rate;

        // Everything built on what was just taken has a smaller package now.
        for (const uint256& d : descendants_of(roots)) {
            const auto q = queued.find(d);
            if (q == queued.end()) continue;
            queue.erase(q->second);
            q->second = package(d);
            queue.insert(q->second);
        }
    }
    return selection;
}

FeeRate Mempool::next_block_fee_rate(std::size_t max_bytes) const {
    const Selection selection = select(max_bytes);
    if (selection.all) return options_.min_relay_fee;
    return std::max(options_.min_relay_fee, FeeRate{selection.lowest.per_kb + 1000});
}

std::size_t Mempool::expire(std::int64_t now) {
    std::vector<uint256> old;
    for (const auto& [id, e] : entries_) {
        if (now - e.time > options_.expiry) old.push_back(id);
    }
    const std::size_t before = entries_.size();
    for (const uint256& id : old) remove_with_descendants(id);
    return before - entries_.size();
}

void Mempool::remove_listener(MempoolListener& listener) { std::erase(listeners_, &listener); }

void Mempool::block_connected(const Block& block, const BlockIndex&) {
    for (const Transaction& tx : block.txs) {
        if (tx.is_coinbase()) continue;
        remove_one(tx.txid());  // confirmed now; whatever spends its outputs can stay
        for (const OutPoint& in : tx.inputs) {
            if (const auto found = spenders_.find(in); found != spenders_.end()) remove_with_descendants(found->second);
        }
    }
}

void Mempool::block_disconnected(const Block& block, const BlockIndex&) {
    std::vector<Transaction> txs;
    for (const Transaction& tx : block.txs) {
        if (!tx.is_coinbase()) txs.push_back(tx);
    }
    disconnected_.push_back(std::move(txs));
}

// After a reorganisation, everything is checked again from scratch: the disconnected blocks'
// transactions (oldest block first), then what was already waiting, which may spend them. Some
// of either may no longer be valid: in the new chain already, double-spent by it, or spending a
// coinbase that's immature again.
void Mempool::tip_changed(const BlockIndex& tip) {
    if (disconnected_.empty()) return;
    std::vector<std::pair<Transaction, std::int64_t>> again;
    for (auto block = disconnected_.rbegin(); block != disconnected_.rend(); ++block) {
        for (Transaction& tx : *block) again.emplace_back(std::move(tx), static_cast<std::int64_t>(tip.header.time));
    }
    for (const MempoolEntry* e : entries()) again.emplace_back(e->tx, e->time);
    disconnected_.clear();
    entries_.clear();
    by_wtxid_.clear();
    spenders_.clear();
    bytes_ = 0;
    fees_ = 0;
    for (const auto& [tx, time] : again) add(tx, time, false);
}

}  // namespace bound
