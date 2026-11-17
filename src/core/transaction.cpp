#include "core/transaction.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace bound {

namespace {

constexpr std::size_t kInputSize = 32 + 4;
constexpr std::size_t kOutputSize = 8 + 32;

void write_body(const Transaction& tx, ByteWriter& w) {
    w.i32(tx.version);
    w.compact_size(tx.inputs.size());
    for (const OutPoint& in : tx.inputs) {
        w.bytes(in.txid.bytes());
        w.u32(in.index);
    }
    w.compact_size(tx.outputs.size());
    for (const TxOut& out : tx.outputs) {
        w.i64(out.amount);
        w.bytes(out.recipient);
    }
    w.u32(tx.lock_time);
    w.var_bytes(tx.memo);
}

template <std::size_t N>
std::array<std::uint8_t, N> read_array(ByteReader& r) {
    const ByteSpan s = r.bytes(N);
    std::array<std::uint8_t, N> out;
    std::copy(s.begin(), s.end(), out.begin());
    return out;
}

}  // namespace

Bytes Transaction::serialize_body() const {
    ByteWriter w;
    write_body(*this, w);
    return std::move(w).take();
}

void Transaction::write(ByteWriter& w) const {
    if (signatures.size() != inputs.size()) throw std::logic_error("need exactly one signature per input");
    write_body(*this, w);
    for (const Signature& s : signatures) w.bytes(s);
}

Bytes Transaction::serialize() const {
    ByteWriter w;
    write(w);
    return std::move(w).take();
}

Transaction Transaction::read(ByteReader& r) {
    Transaction tx;
    tx.version = r.i32();
    const std::uint64_t n_in = r.compact_size();
    if (n_in > r.remaining() / kInputSize) throw DeserializeError("input count too large");
    tx.inputs.resize(static_cast<std::size_t>(n_in));
    for (OutPoint& in : tx.inputs) {
        in.txid = uint256(read_array<32>(r));
        in.index = r.u32();
    }
    const std::uint64_t n_out = r.compact_size();
    if (n_out > r.remaining() / kOutputSize) throw DeserializeError("output count too large");
    tx.outputs.resize(static_cast<std::size_t>(n_out));
    for (TxOut& out : tx.outputs) {
        out.amount = r.i64();
        out.recipient = read_array<32>(r);
    }
    tx.lock_time = r.u32();
    tx.memo = r.var_bytes(kMaxMemo);
    tx.signatures.resize(tx.inputs.size());
    for (Signature& s : tx.signatures) s = read_array<64>(r);
    return tx;
}

Transaction Transaction::deserialize(ByteSpan data) {
    ByteReader r(data);
    Transaction tx = read(r);
    r.expect_end();
    return tx;
}

uint256 Transaction::txid() const { return uint256(tagged_hash("BOUND/txid", serialize_body())); }
uint256 Transaction::wtxid() const { return uint256(tagged_hash("BOUND/wtxid", serialize())); }
uint256 Transaction::sighash() const { return uint256(tagged_hash("BOUND/sighash", txid().bytes())); }

void sign_all_inputs(Transaction& tx, const KeyPair& key) {
    const Signature sig = key.sign(tx.sighash().bytes());
    tx.signatures.assign(tx.inputs.size(), sig);
}

bool verify_input(const Transaction& tx, std::size_t index, const PublicKey& owner) {
    if (index >= tx.inputs.size() || index >= tx.signatures.size()) return false;
    return verify(owner, tx.sighash().bytes(), tx.signatures[index]);
}

}  // namespace bound
