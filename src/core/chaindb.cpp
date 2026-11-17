#include "core/chaindb.h"

#include "util/serialize.h"

#include <algorithm>
#include <climits>
#include <utility>

namespace bound {

namespace {

constexpr std::size_t kCoinSize = 45;

Bytes key_with_hash(std::uint8_t prefix, const uint256& hash) {
    ByteWriter w;
    w.u8(prefix);
    w.bytes(hash.bytes());
    return std::move(w).take();
}

uint256 read_hash(ByteReader& r) {
    const ByteSpan b = r.bytes(32);
    Digest32 d;
    std::copy(b.begin(), b.end(), d.begin());
    return uint256(d);
}

int read_height(ByteReader& r) {
    const std::uint32_t h = r.u32();
    if (h > INT_MAX) throw DeserializeError("height out of range");
    return static_cast<int>(h);
}

void write_coin(ByteWriter& w, const Coin& coin) {
    w.i64(coin.out.amount);
    w.bytes(coin.out.recipient);
    w.u32(static_cast<std::uint32_t>(coin.height));
    w.u8(coin.coinbase ? 1 : 0);
}

Coin read_coin(ByteReader& r) {
    Coin coin;
    coin.out.amount = r.i64();
    const ByteSpan recipient = r.bytes(coin.out.recipient.size());
    std::copy(recipient.begin(), recipient.end(), coin.out.recipient.begin());
    coin.height = read_height(r);
    const std::uint8_t coinbase = r.u8();
    if (coinbase > 1) throw DeserializeError("bad coinbase flag");
    coin.coinbase = coinbase == 1;
    return coin;
}

}  // namespace

Bytes meta_key() { return {'V'}; }
Bytes tip_key() { return {'T'}; }
Bytes index_key(const uint256& block_hash) { return key_with_hash(kIndexPrefix, block_hash); }
Bytes undo_key(const uint256& block_hash) { return key_with_hash('U', block_hash); }

Bytes coin_key(const OutPoint& op) {
    ByteWriter w;
    w.u8(kCoinPrefix);
    w.bytes(op.txid.bytes());
    w.u32(op.index);
    return std::move(w).take();
}

OutPoint decode_coin_key(ByteSpan key) {
    ByteReader r(key);
    if (r.u8() != kCoinPrefix) throw DeserializeError("not a coin key");
    OutPoint op;
    op.txid = read_hash(r);
    op.index = r.u32();
    r.expect_end();
    return op;
}

Bytes encode_meta(const ChainMeta& meta) {
    ByteWriter w;
    w.u32(meta.version);
    w.bytes(meta.genesis.bytes());
    w.var_bytes(bytes_of(meta.network));
    return std::move(w).take();
}

ChainMeta decode_meta(ByteSpan data) {
    ByteReader r(data);
    ChainMeta meta;
    meta.version = r.u32();
    meta.genesis = read_hash(r);
    const Bytes network = r.var_bytes(64);
    meta.network.assign(network.begin(), network.end());
    r.expect_end();
    return meta;
}

Bytes encode_hash(const uint256& hash) { return Bytes(hash.bytes().begin(), hash.bytes().end()); }

uint256 decode_hash(ByteSpan data) {
    ByteReader r(data);
    const uint256 hash = read_hash(r);
    r.expect_end();
    return hash;
}

Bytes encode_index(const BlockIndex& node) {
    ByteWriter w;
    w.bytes(node.header.serialize());
    w.u32(static_cast<std::uint32_t>(node.height));
    w.u8(static_cast<std::uint8_t>((node.have_data ? 1 : 0) | (node.invalid ? 2 : 0)));
    w.u8(static_cast<std::uint8_t>(node.invalid_reason));
    w.u64(node.sequence);
    w.u32(node.pos.file);
    w.u32(node.pos.offset);
    return std::move(w).take();
}

BlockIndex decode_index(ByteSpan data) {
    ByteReader r(data);
    BlockIndex node;
    node.header = BlockHeader::deserialize(r.bytes(BlockHeader::kSize));
    node.hash = node.header.hash();
    node.height = read_height(r);
    const std::uint8_t flags = r.u8();
    if (flags > 3) throw DeserializeError("bad block index flags");
    node.have_data = (flags & 1) != 0;
    node.invalid = (flags & 2) != 0;
    const std::uint8_t reason = r.u8();
    if (reason > static_cast<std::uint8_t>(ConnectResult::CoinbaseTooLarge)) throw DeserializeError("bad invalid_reason");
    node.invalid_reason = static_cast<ConnectResult>(reason);
    node.sequence = r.u64();
    node.pos.file = r.u32();
    node.pos.offset = r.u32();
    r.expect_end();
    return node;
}

Bytes encode_coin(const Coin& coin) {
    ByteWriter w;
    write_coin(w, coin);
    return std::move(w).take();
}

Coin decode_coin(ByteSpan data) {
    ByteReader r(data);
    const Coin coin = read_coin(r);
    r.expect_end();
    return coin;
}

Bytes encode_undo(const BlockUndo& undo) {
    ByteWriter w;
    w.compact_size(undo.spent.size());
    for (const Coin& coin : undo.spent) write_coin(w, coin);
    return std::move(w).take();
}

BlockUndo decode_undo(ByteSpan data) {
    ByteReader r(data);
    const std::uint64_t count = r.compact_size();
    if (count > r.remaining() / kCoinSize) throw DeserializeError("undo coin count exceeds the data");
    BlockUndo undo;
    undo.spent.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) undo.spent.push_back(read_coin(r));
    r.expect_end();
    return undo;
}

}  // namespace bound
