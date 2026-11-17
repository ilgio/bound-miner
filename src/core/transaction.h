#pragma once

#include "core/amount.h"
#include "core/uint256.h"
#include "crypto/ed25519.h"
#include "util/bytes.h"
#include "util/serialize.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bound {

// Output `index` of transaction `txid`.
struct OutPoint {
    uint256 txid;
    std::uint32_t index = 0;

    friend bool operator==(const OutPoint&, const OutPoint&) = default;
    friend std::strong_ordering operator<=>(const OutPoint&, const OutPoint&) = default;
};

// `amount` base units, spendable by whoever can sign with `recipient`. There is no script
// language: the only spending condition is an Ed25519 signature by that key (pay-to-public-key,
// like a Taproot key-path spend).
struct TxOut {
    Amount amount = 0;
    PublicKey recipient{};

    friend bool operator==(const TxOut&, const TxOut&) = default;
};

struct Transaction {
    static constexpr std::size_t kMaxMemo = 100;

    std::int32_t version = 1;
    std::vector<OutPoint> inputs;  // empty means coinbase
    std::vector<TxOut> outputs;
    // Coinbase: the block height, which makes every coinbase txid unique. Others: 0 for now
    // (reserved for timelocks).
    std::uint32_t lock_time = 0;
    // Free-form, at most kMaxMemo bytes. Miners vary it in the coinbase as an extra nonce.
    Bytes memo;
    // signatures[i] authorizes inputs[i]. This is the witness: covered by wtxid, not by txid.
    std::vector<Signature> signatures;

    bool is_coinbase() const { return inputs.empty(); }

    // Body, the part txid covers:
    //   version(i32) compact_size(#inputs) {txid(32) index(u32)}*
    //   compact_size(#outputs) {amount(i64) recipient(32)}* lock_time(u32) var_bytes(memo)
    // Full serialization = body, then exactly #inputs signatures of 64 bytes, no count prefix.
    Bytes serialize_body() const;
    // Full serialization. Throws std::logic_error unless signatures.size() == inputs.size().
    Bytes serialize() const;
    void write(ByteWriter& w) const;  // appends the full serialization; same precondition

    // Inverse of serialize(). Throws DeserializeError on truncated input, trailing bytes, a memo
    // over kMaxMemo, or an input/output count that can't fit in the bytes left (checked before
    // allocating anything: a 5-byte count must not reserve gigabytes).
    static Transaction deserialize(ByteSpan data);
    // The same, reading one transaction out of a larger message (no end-of-input check).
    static Transaction read(ByteReader& r);

    uint256 txid() const;     // uint256(tagged_hash("BOUND/txid", serialize_body()))
    uint256 wtxid() const;    // uint256(tagged_hash("BOUND/wtxid", serialize())); the merkle leaves
    uint256 sighash() const;  // uint256(tagged_hash("BOUND/sighash", txid().bytes())): what every
                              // input's signature signs. One sighash type: it covers everything.

    friend bool operator==(const Transaction&, const Transaction&) = default;
};

// Single-key wallet signing: sets tx.signatures to one key.sign(sighash) per input.
void sign_all_inputs(Transaction& tx, const KeyPair& key);

// True iff tx.signatures[index] is owner's valid signature of tx.sighash(). Returns false (no UB)
// when the input or its signature doesn't exist. Milestone 3 calls this with the spent output's
// recipient.
bool verify_input(const Transaction& tx, std::size_t index, const PublicKey& owner);

}  // namespace bound
