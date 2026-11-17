#pragma once

#include "core/transaction.h"
#include "core/uint256.h"
#include "util/bytes.h"
#include "util/serialize.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bound {

// Field-for-field identical to Bitcoin's header, so any real Bitcoin header is a test vector.
// Wire format, 80 bytes: version(i32) prev_hash(32) merkle_root(32) time(u32) bits(u32)
// nonce(u32); integers little-endian, hashes as their raw bytes().
struct BlockHeader {
    static constexpr std::size_t kSize = 80;

    std::int32_t version = 1;
    uint256 prev_hash;
    uint256 merkle_root;
    std::uint32_t time = 0;  // Unix seconds
    std::uint32_t bits = 0;  // compact target, see pow.h
    std::uint32_t nonce = 0;

    std::array<std::uint8_t, kSize> serialize() const;
    // Throws DeserializeError unless data is exactly 80 bytes.
    static BlockHeader deserialize(ByteSpan data);
    // uint256(sha256d(serialize())): the block's id. Until milestone 16 it was also the number proof
    // of work constrains; now that's pow_hash (core/pow.h), BoundHash.
    uint256 hash() const;

    friend bool operator==(const BlockHeader&, const BlockHeader&) = default;
};

// Milestone 2: a header plus its transactions. txs[0] must be the coinbase (see check_block).
struct Block {
    BlockHeader header;
    std::vector<Transaction> txs;

    // header (80 bytes), compact_size(#txs), then each transaction's full serialization.
    Bytes serialize() const;
    // Throws DeserializeError on malformed input, trailing bytes, or a transaction count that
    // can't fit in the bytes left (a transaction is at least 11 bytes).
    static Block deserialize(ByteSpan data);

    uint256 hash() const { return header.hash(); }
    // merkle_root over the txs' wtxids: the value header.merkle_root must equal.
    uint256 compute_merkle_root() const;

    friend bool operator==(const Block&, const Block&) = default;
};

}  // namespace bound
