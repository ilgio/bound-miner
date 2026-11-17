#include "core/block.h"

#include "core/merkle.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace bound {

namespace {
uint256 read_hash(ByteReader& r) {
    const ByteSpan s = r.bytes(32);
    Digest32 d;
    std::copy(s.begin(), s.end(), d.begin());
    return uint256(d);
}
}  // namespace

std::array<std::uint8_t, BlockHeader::kSize> BlockHeader::serialize() const {
    ByteWriter w;
    w.i32(version);
    w.bytes(prev_hash.bytes());
    w.bytes(merkle_root.bytes());
    w.u32(time);
    w.u32(bits);
    w.u32(nonce);
    std::array<std::uint8_t, kSize> out;
    std::copy(w.data().begin(), w.data().end(), out.begin());
    return out;
}

BlockHeader BlockHeader::deserialize(ByteSpan data) {
    if (data.size() != kSize) throw DeserializeError("block header must be 80 bytes");
    ByteReader r(data);
    BlockHeader h;
    h.version = r.i32();
    h.prev_hash = read_hash(r);
    h.merkle_root = read_hash(r);
    h.time = r.u32();
    h.bits = r.u32();
    h.nonce = r.u32();
    r.expect_end();
    return h;
}

uint256 BlockHeader::hash() const { return uint256(sha256d(serialize())); }

Bytes Block::serialize() const {
    ByteWriter w;
    w.bytes(header.serialize());
    w.compact_size(txs.size());
    for (const Transaction& tx : txs) tx.write(w);
    return std::move(w).take();
}

Block Block::deserialize(ByteSpan data) {
    constexpr std::size_t kMinTxSize = 11;
    ByteReader r(data);
    Block b;
    b.header = BlockHeader::deserialize(r.bytes(BlockHeader::kSize));
    const std::uint64_t n = r.compact_size();
    if (n > r.remaining() / kMinTxSize) throw DeserializeError("transaction count too large");
    b.txs.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) b.txs.push_back(Transaction::read(r));
    r.expect_end();
    return b;
}

uint256 Block::compute_merkle_root() const {
    std::vector<uint256> leaves;
    leaves.reserve(txs.size());
    for (const Transaction& tx : txs) leaves.push_back(tx.wtxid());
    return merkle_root(leaves);
}

}  // namespace bound
