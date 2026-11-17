#pragma once

#include "core/storage.h"

#include <filesystem>
#include <memory>

namespace leveldb {
class DB;
}

namespace bound {

// Milestone 5: a KeyValueStore on LevelDB, the database Bitcoin Core keeps its block index and
// UTXO set in. Every write is one synced leveldb::WriteBatch, so it's atomic and durable. LevelDB
// locks its directory: a second store on the same directory, from this process or another, fails
// to open.
class LevelDbStore final : public KeyValueStore {
public:
    // Opens the database in `dir`, creating it if needed. Throws std::runtime_error on failure.
    explicit LevelDbStore(const std::filesystem::path& dir);
    ~LevelDbStore() override;
    LevelDbStore(const LevelDbStore&) = delete;
    LevelDbStore& operator=(const LevelDbStore&) = delete;

    std::optional<Bytes> get(ByteSpan key) const override;
    void write(const WriteBatch& batch) override;
    void scan(ByteSpan prefix, bool reverse, const std::function<bool(ByteSpan key, ByteSpan value)>& f) const override;

private:
    std::unique_ptr<leveldb::DB> db_;
};

}  // namespace bound
