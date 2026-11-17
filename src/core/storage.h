#pragma once

#include "core/block.h"
#include "core/chain.h"
#include "util/bytes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace bound {

// Milestone 5: where the chainstate keeps what must survive a restart. Two pieces, as in Bitcoin
// Core: a key-value database for the block tree, the UTXO set and undo data, and append-only
// files for the blocks themselves. Each comes in a memory version (tests, `boundd demo`) and a
// disk version (LevelDbStore, FlatFileBlockStore).

// Puts and erases to apply together.
class WriteBatch {
public:
    struct Op {
        Bytes key;
        std::optional<Bytes> value;  // nullopt: erase the key
    };

    void put(Bytes key, Bytes value) { ops_.push_back({std::move(key), std::move(value)}); }
    void erase(Bytes key) { ops_.push_back({std::move(key), std::nullopt}); }
    const std::vector<Op>& ops() const { return ops_; }
    bool empty() const { return ops_.empty(); }

private:
    std::vector<Op> ops_;
};

class KeyValueStore {
public:
    virtual ~KeyValueStore() = default;

    virtual std::optional<Bytes> get(ByteSpan key) const = 0;
    // Applies the operations in order (a later one on the same key wins), all or none, and
    // durably: once write returns, a crash can't undo it. Throws std::runtime_error on failure.
    virtual void write(const WriteBatch& batch) = 0;
    // Milestone 8. Calls f(key, value) for the keys that start with `prefix`: in byte order of the
    // keys, or from the last to the first if `reverse`. Stops early when f returns false, so
    // "the newest 25 of a million" reads 25.
    virtual void scan(ByteSpan prefix, bool reverse, const std::function<bool(ByteSpan key, ByteSpan value)>& f) const = 0;
    // All of them, in byte order.
    void for_each(ByteSpan prefix, const std::function<void(ByteSpan key, ByteSpan value)>& f) const {
        scan(prefix, false, [&](ByteSpan key, ByteSpan value) {
            f(key, value);
            return true;
        });
    }
};

// The smallest key greater than every key that starts with `prefix` (the prefix with its last
// byte that isn't 0xff incremented, and what follows cut off). nullopt if there's none: an empty
// prefix, or all 0xff. A reverse scan starts just before it.
std::optional<Bytes> prefix_successor(ByteSpan prefix);

class MemoryStore final : public KeyValueStore {
public:
    std::optional<Bytes> get(ByteSpan key) const override;
    void write(const WriteBatch& batch) override;
    void scan(ByteSpan prefix, bool reverse, const std::function<bool(ByteSpan key, ByteSpan value)>& f) const override;

private:
    // Byte order, by hand: GCC 13 at -O2 warns falsely (-Wstringop-overread) inside the standard
    // library's vector<unsigned char> comparison. This store is for tests; speed doesn't matter.
    struct ByteOrder {
        bool operator()(const Bytes& a, const Bytes& b) const {
            const std::size_t n = a.size() < b.size() ? a.size() : b.size();
            for (std::size_t i = 0; i < n; ++i) {
                if (a[i] != b[i]) return a[i] < b[i];
            }
            return a.size() < b.size();
        }
    };
    std::map<Bytes, Bytes, ByteOrder> data_;
};

class BlockStore {
public:
    virtual ~BlockStore() = default;

    // Stores a block and returns where. Durable once it returns, so an index entry pointing here
    // may be written next. Throws std::runtime_error on failure.
    virtual BlockPos append(const Block& block) = 0;
    // Throws std::runtime_error (DeserializeError included) if pos doesn't hold a stored block.
    virtual Block read(BlockPos pos) const = 0;
};

class MemoryBlockStore final : public BlockStore {
public:
    BlockPos append(const Block& block) override;
    Block read(BlockPos pos) const override;

private:
    std::vector<Bytes> blocks_;  // pos.offset indexes this; pos.file is always 0
};

// Bitcoin's blk?????.dat layout: blocks appended to numbered files, a new file once one reaches
// max_file_size. Each record is the magic "BOND", the block's length (u32) and its bytes; a
// BlockPos points at the magic. A crash mid-append leaves junk at the end of the last file, which
// nothing points at; appending simply carries on after it.
class FlatFileBlockStore final : public BlockStore {
public:
    static constexpr std::uint32_t kDefaultMaxFileSize = 128 << 20;

    // Creates `dir` if needed and continues after the last file already in it.
    explicit FlatFileBlockStore(std::filesystem::path dir, std::uint32_t max_file_size = kDefaultMaxFileSize);

    BlockPos append(const Block& block) override;
    Block read(BlockPos pos) const override;

    std::filesystem::path file_path(std::uint32_t file) const;  // dir/blk00000.dat, ...

private:
    std::filesystem::path dir_;
    std::uint32_t max_file_size_;
    std::uint32_t file_ = 0;  // the file being appended to
    std::uint32_t size_ = 0;  // its current length
};

}  // namespace bound
