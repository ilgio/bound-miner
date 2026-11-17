#include "core/leveldb_store.h"

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include <stdexcept>
#include <string>

namespace bound {

namespace {

// An empty span may carry a null pointer, which LevelDB hands to memcmp (undefined even for 0
// bytes; UBSan caught it in milestone 12). LevelDB's own empty Slice points at "".
leveldb::Slice slice(ByteSpan b) {
    if (b.empty()) return {};
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

ByteSpan span(const leveldb::Slice& s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }

void check(const leveldb::Status& status, const char* what) {
    if (!status.ok()) throw std::runtime_error(std::string(what) + ": " + status.ToString());
}

}  // namespace

LevelDbStore::LevelDbStore(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    leveldb::Options options;
    options.create_if_missing = true;
    // Milestone 14: written uncompressed, as Bitcoin Core does, so that any LevelDB can read it.
    // LevelDB compresses with Snappy by default when it was built with it (the Linux
    // distributions' builds are) and can't read Snappy blocks when it wasn't (vcpkg's, until its
    // "snappy" feature was turned on). Hashes and keys barely compress anyway. Blocks already
    // written compressed are still read: every release build has Snappy (tests/test_storage.cpp).
    options.compression = leveldb::kNoCompression;
    leveldb::DB* db = nullptr;
    check(leveldb::DB::Open(options, dir.string(), &db), "can't open the database");
    db_.reset(db);
}

LevelDbStore::~LevelDbStore() = default;

std::optional<Bytes> LevelDbStore::get(ByteSpan key) const {
    std::string value;
    const leveldb::Status status = db_->Get(leveldb::ReadOptions(), slice(key), &value);
    if (status.IsNotFound()) return std::nullopt;
    check(status, "database read failed");
    const ByteSpan bytes = span(value);
    return Bytes(bytes.begin(), bytes.end());
}

void LevelDbStore::write(const WriteBatch& batch) {
    leveldb::WriteBatch b;
    for (const WriteBatch::Op& op : batch.ops()) {
        if (op.value) {
            b.Put(slice(op.key), slice(*op.value));
        } else {
            b.Delete(slice(op.key));
        }
    }
    leveldb::WriteOptions options;
    options.sync = true;
    check(db_->Write(options, &b), "database write failed");
}

void LevelDbStore::scan(ByteSpan prefix, bool reverse, const std::function<bool(ByteSpan key, ByteSpan value)>& f) const {
    const std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(leveldb::ReadOptions()));
    if (!reverse) {
        it->Seek(slice(prefix));
    } else if (const std::optional<Bytes> after = prefix_successor(prefix)) {
        // The last key before the first one past the prefix.
        it->Seek(slice(*after));
        if (it->Valid()) {
            it->Prev();
        } else {
            it->SeekToLast();
        }
    } else {
        it->SeekToLast();
    }
    while (it->Valid() && it->key().starts_with(slice(prefix)) && f(span(it->key()), span(it->value()))) {
        if (reverse) {
            it->Prev();
        } else {
            it->Next();
        }
    }
    check(it->status(), "database read failed");
}

}  // namespace bound
