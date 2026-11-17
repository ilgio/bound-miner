#pragma once

#include "core/chain.h"
#include "core/coins.h"
#include "core/transaction.h"
#include "core/uint256.h"
#include "util/bytes.h"

#include <cstdint>
#include <string>

namespace bound {

// Milestone 5: how the chainstate lays itself out in a KeyValueStore. The first byte of a key says
// what it holds:
//   'V'                   version, genesis hash, network name: which chain this database holds
//   'T'                   hash of the active tip; the stored UTXO set is the one at this block
//   'B' + block hash      a BlockIndex record, for every header in the tree
//   'C' + txid + index    an unspent Coin
//   'U' + block hash      the BlockUndo of a block that is, or once was, on the active chain
// Integers are little-endian, as on the wire. Every decode throws DeserializeError on malformed
// input.

inline constexpr std::uint32_t kChainDbVersion = 1;
inline constexpr std::uint8_t kIndexPrefix = 'B';
inline constexpr std::uint8_t kCoinPrefix = 'C';

Bytes meta_key();
Bytes tip_key();
Bytes index_key(const uint256& block_hash);
Bytes coin_key(const OutPoint& op);
Bytes undo_key(const uint256& block_hash);
OutPoint decode_coin_key(ByteSpan key);

struct ChainMeta {
    std::uint32_t version = kChainDbVersion;
    uint256 genesis;
    std::string network;

    friend bool operator==(const ChainMeta&, const ChainMeta&) = default;
};
Bytes encode_meta(const ChainMeta& meta);  // version u32, genesis hash, var_bytes network name
ChainMeta decode_meta(ByteSpan data);

Bytes encode_hash(const uint256& hash);  // its 32 bytes
uint256 decode_hash(ByteSpan data);

// What a BlockIndex can't recompute: header (80 bytes), height u32, flags u8 (1: have_data,
// 2: invalid), invalid_reason u8, sequence u64, pos.file u32, pos.offset u32. The parent and
// children pointers, chain work and `linked` are rebuilt when the tree is loaded.
Bytes encode_index(const BlockIndex& node);
BlockIndex decode_index(ByteSpan data);  // hash is set to header.hash()

// amount i64, recipient (32 bytes), height u32, coinbase u8: 45 bytes.
Bytes encode_coin(const Coin& coin);
Coin decode_coin(ByteSpan data);

// compact_size count, then each spent coin as encode_coin writes it.
Bytes encode_undo(const BlockUndo& undo);
BlockUndo decode_undo(ByteSpan data);

}  // namespace bound
