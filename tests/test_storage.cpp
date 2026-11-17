#include "core/chaindb.h"
#include "core/leveldb_store.h"
#include "core/miner.h"
#include "core/storage.h"
#include "test_util.h"
#include "util/hex.h"
#include "util/serialize.h"

#include <gtest/gtest.h>
#include <leveldb/db.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace bound;

namespace {

Bytes B(std::string_view s) {
    const ByteSpan b = bytes_of(s);
    return Bytes(b.begin(), b.end());
}

std::vector<std::string> keys_with_prefix(const KeyValueStore& store, std::string_view prefix) {
    std::vector<std::string> keys;
    store.for_each(bytes_of(prefix), [&](ByteSpan key, ByteSpan) { keys.emplace_back(key.begin(), key.end()); });
    return keys;
}

// The contract every KeyValueStore keeps.
void check_store_contract(KeyValueStore& store) {
    EXPECT_EQ(store.get(B("missing")), std::nullopt);

    WriteBatch batch;
    batch.put(B("Ca"), B("1"));
    batch.put(B("Cb"), B("2"));
    batch.put(B("B"), B("3"));
    batch.put(B("C"), B("4"));
    batch.put(B("Cc"), B("5"));
    batch.erase(B("Cc"));        // a later operation on a key wins: Cc ends up absent...
    batch.put(B("Ca"), B("6"));  // ...and Ca ends up 6
    store.write(batch);
    EXPECT_EQ(store.get(B("Ca")), B("6"));
    EXPECT_EQ(store.get(B("Cc")), std::nullopt);
    EXPECT_EQ(keys_with_prefix(store, "C"), (std::vector<std::string>{"C", "Ca", "Cb"}));
    EXPECT_EQ(keys_with_prefix(store, "D"), std::vector<std::string>{});

    // Milestone 8: scans run either way and can stop early.
    const auto scan = [&](ByteSpan prefix, bool reverse, std::size_t limit) {
        std::vector<Bytes> keys;
        store.scan(prefix, reverse, [&](ByteSpan k, ByteSpan) {
            keys.emplace_back(k.begin(), k.end());
            return keys.size() < limit;
        });
        return keys;
    };
    EXPECT_EQ(scan(bytes_of("C"), true, 10), (std::vector<Bytes>{B("Cb"), B("Ca"), B("C")}));
    EXPECT_EQ(scan(bytes_of("C"), true, 2), (std::vector<Bytes>{B("Cb"), B("Ca")}));
    EXPECT_EQ(scan(bytes_of("C"), false, 1), (std::vector<Bytes>{B("C")}));
    EXPECT_EQ(scan(bytes_of("Ca"), true, 10), (std::vector<Bytes>{B("Ca")}));
    EXPECT_EQ(scan(bytes_of("B"), true, 10), (std::vector<Bytes>{B("B")}));  // stops at the prefix's edge
    EXPECT_TRUE(scan(bytes_of("D"), true, 10).empty());
    EXPECT_EQ(scan({}, true, 2), (std::vector<Bytes>{B("Cb"), B("Ca")}));  // no prefix: from the very last key

    // Keys and values are bytes, not strings: zeros and 0xff included.
    const Bytes key{0x00, 0xff, 0x00};
    const Bytes value{0xff, 0x00};
    WriteBatch binary;
    binary.put(key, value);
    binary.put(Bytes{0xff, 0xff}, B("x"));
    binary.put(Bytes{0xff, 0xff, 0x01}, B("y"));
    store.write(binary);
    EXPECT_EQ(store.get(key), value);
    EXPECT_EQ(scan(Bytes{0x00, 0xff}, true, 10), (std::vector<Bytes>{key}));
    // A prefix of all 0xff has no key after it to start a reverse scan from.
    EXPECT_EQ(scan(Bytes{0xff, 0xff}, true, 10), (std::vector<Bytes>{Bytes{0xff, 0xff, 0x01}, Bytes{0xff, 0xff}}));
    EXPECT_EQ(scan(Bytes{0xff}, false, 10), (std::vector<Bytes>{Bytes{0xff, 0xff}, Bytes{0xff, 0xff, 0x01}}));

    WriteBatch erase;
    erase.erase(B("Cb"));
    erase.erase(B("never existed"));
    store.write(erase);
    EXPECT_EQ(store.get(B("Cb")), std::nullopt);
}

// Small distinct blocks, for the block stores.
Block sample_block(std::string_view memo) {
    Block b;
    b.txs.push_back(make_coinbase(1, 5 * kCoin, test_key("alice").public_key(), bytes_of(memo)));
    b.header.merkle_root = b.compute_merkle_root();
    b.header.time = kTestGenesisTime;
    return b;
}

std::uint32_t record_size(const Block& b) { return static_cast<std::uint32_t>(8 + b.serialize().size()); }

}  // namespace

TEST(MemoryStore, KeepsTheContract) {
    MemoryStore store;
    check_store_contract(store);
}

TEST(Storage, PrefixSuccessor) {
    EXPECT_EQ(prefix_successor(Bytes{'a'}), Bytes{'b'});
    EXPECT_EQ(prefix_successor(Bytes{'a', 0x00}), (Bytes{'a', 0x01}));
    EXPECT_EQ(prefix_successor(Bytes{'a', 0xff}), Bytes{'b'});
    EXPECT_EQ(prefix_successor(Bytes{0x01, 0xff, 0xff}), Bytes{0x02});
    EXPECT_EQ(prefix_successor(Bytes{0xff, 0xff}), std::nullopt);
    EXPECT_EQ(prefix_successor(Bytes{}), std::nullopt);
}

TEST(LevelDbStore, KeepsTheContract) {
    TempDir dir;
    LevelDbStore store(dir.path() / "db");
    check_store_contract(store);
}

TEST(LevelDbStore, SurvivesReopening) {
    TempDir dir;
    {
        LevelDbStore store(dir.path() / "db");
        WriteBatch batch;
        batch.put(B("k"), B("v"));
        store.write(batch);
    }
    const LevelDbStore store(dir.path() / "db");
    EXPECT_EQ(store.get(B("k")), B("v"));
}

// Milestone 14: a node built against a Linux distribution's LevelDB wrote its chain compressed with
// Snappy. Every build of ours has to read that (vcpkg's LevelDB couldn't, until its "snappy"
// feature was turned on), or such a node can't move to a release package.
TEST(LevelDbStore, ReadsWhatASnappyLevelDbWrote) {
    TempDir dir;
    const std::filesystem::path path = dir.path() / "db";
    const std::string value(4000, 'x');
    {
        leveldb::Options options;
        options.create_if_missing = true;
        options.compression = leveldb::kSnappyCompression;
        leveldb::DB* raw = nullptr;
        ASSERT_TRUE(leveldb::DB::Open(options, path.string(), &raw).ok());
        const std::unique_ptr<leveldb::DB> db(raw);
        for (int i = 0; i < 50; ++i) ASSERT_TRUE(db->Put(leveldb::WriteOptions(), "key" + std::to_string(i), value).ok());
        db->CompactRange(nullptr, nullptr);  // out of the log, into compressed table files
    }
    // The tables came out far smaller than the 200 KB put in: Snappy is really there.
    std::uintmax_t tables = 0;
    for (const auto& file : std::filesystem::directory_iterator(path)) {
        if (file.path().extension() == ".ldb") tables += file.file_size();
    }
    ASSERT_GT(tables, 0u);
    EXPECT_LT(tables, 50u * value.size() / 4);

    const LevelDbStore store(path);
    EXPECT_EQ(store.get(B("key7")), Bytes(value.begin(), value.end()));
}

TEST(LevelDbStore, OneOpenAtATime) {
    TempDir dir;
    const LevelDbStore store(dir.path() / "db");
    EXPECT_THROW({ const LevelDbStore second(dir.path() / "db"); }, std::runtime_error);
}

TEST(MemoryBlockStore, StoresAndReads) {
    MemoryBlockStore store;
    const BlockPos a = store.append(sample_block("a"));
    const BlockPos b = store.append(sample_block("b"));
    EXPECT_EQ(store.read(a), sample_block("a"));
    EXPECT_EQ(store.read(b), sample_block("b"));
    EXPECT_THROW(store.read({0, 7}), std::runtime_error);
}

TEST(FlatFileBlockStore, AppendsRecords) {
    TempDir dir;
    FlatFileBlockStore store(dir.path());
    const Block a = sample_block("a");
    const Block b = sample_block("b");
    const BlockPos pa = store.append(a);
    const BlockPos pb = store.append(b);
    EXPECT_EQ(pa, (BlockPos{0, 0}));
    EXPECT_EQ(pb, (BlockPos{0, record_size(a)}));  // after a's magic, length and bytes
    EXPECT_EQ(store.read(pa), a);
    EXPECT_EQ(store.read(pb), b);
    EXPECT_EQ(std::filesystem::file_size(store.file_path(0)), record_size(a) + record_size(b));
}

TEST(FlatFileBlockStore, StartsANewFileWhenOneIsFull) {
    TempDir dir;
    const Block a = sample_block("a");
    FlatFileBlockStore store(dir.path(), record_size(a));  // room for exactly one block per file
    EXPECT_EQ(store.append(a), (BlockPos{0, 0}));
    EXPECT_EQ(store.append(sample_block("b")), (BlockPos{1, 0}));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "blk00001.dat"));
    EXPECT_EQ(store.read({1, 0}), sample_block("b"));
}

TEST(FlatFileBlockStore, CarriesOnAfterReopening) {
    TempDir dir;
    const Block a = sample_block("a");
    BlockPos pa;
    {
        FlatFileBlockStore store(dir.path());
        pa = store.append(a);
    }
    FlatFileBlockStore store(dir.path());
    const BlockPos pb = store.append(sample_block("b"));
    EXPECT_EQ(pb, (BlockPos{0, record_size(a)}));
    EXPECT_EQ(store.read(pa), a);
    EXPECT_EQ(store.read(pb), sample_block("b"));
}

// A crash halfway through an append leaves junk at the end of the file. Nothing points at it,
// and the next append goes after it.
TEST(FlatFileBlockStore, IgnoresAHalfWrittenRecord) {
    TempDir dir;
    BlockPos pa;
    {
        FlatFileBlockStore store(dir.path());
        pa = store.append(sample_block("a"));
    }
    {
        std::ofstream f(dir.path() / "blk00000.dat", std::ios::binary | std::ios::app);
        f.write("BOND\x40\x00", 6);
    }
    FlatFileBlockStore store(dir.path());
    const BlockPos pb = store.append(sample_block("b"));
    EXPECT_EQ(store.read(pa), sample_block("a"));
    EXPECT_EQ(store.read(pb), sample_block("b"));
}

TEST(FlatFileBlockStore, RejectsWhatIsntAStoredBlock) {
    TempDir dir;
    FlatFileBlockStore store(dir.path());
    const BlockPos pa = store.append(sample_block("a"));
    EXPECT_THROW(store.read({pa.file, pa.offset + 1}), std::runtime_error);  // not at a record
    EXPECT_THROW(store.read({pa.file, 1'000'000}), std::runtime_error);       // past the end
    EXPECT_THROW(store.read({5, 0}), std::runtime_error);                    // no such file
}

TEST(ChainDb, CoinEncoding) {
    const Coin coin{TxOut{5 * kCoin, test_key("alice").public_key()}, 7, true};
    EXPECT_EQ(to_hex(encode_coin(coin)),
              "0065cd1d00000000"                                                  // 5 BOUND
              "d5bf4a3fcce717b0388bcc2749ebc148ad9969b23f45ee1b605fd58778576ac4"  // to alice
              "07000000"                                                          // height 7
              "01");                                                              // a coinbase output
    EXPECT_EQ(decode_coin(encode_coin(coin)), coin);
    EXPECT_THROW(decode_coin(Bytes(44)), DeserializeError);
    Bytes bad_flag = encode_coin(coin);
    bad_flag.back() = 2;
    EXPECT_THROW(decode_coin(bad_flag), DeserializeError);
}

TEST(ChainDb, CoinKeys) {
    const OutPoint op{test_hash("tx"), 3};
    const Bytes key = coin_key(op);
    ASSERT_EQ(key.size(), 37u);
    EXPECT_EQ(key[0], std::uint8_t{'C'});
    EXPECT_EQ(decode_coin_key(key), op);
}

TEST(ChainDb, UndoRoundTrip) {
    BlockUndo undo;
    undo.spent = {Coin{TxOut{3 * kCoin, test_key("alice").public_key()}, 10, false},
                  Coin{TxOut{5 * kCoin, test_key("bob").public_key()}, 2, true}};
    EXPECT_EQ(decode_undo(encode_undo(undo)), undo);
    EXPECT_EQ(decode_undo(encode_undo(BlockUndo{})), BlockUndo{});
    Bytes lying = encode_undo(undo);
    lying[0] = 3;  // claims a third coin that isn't there
    EXPECT_THROW(decode_undo(lying), DeserializeError);
}

TEST(ChainDb, IndexRecordKeepsWhatCantBeRecomputed) {
    BlockIndex node;
    node.header = easy_params().genesis;
    node.hash = node.header.hash();
    node.height = 5;
    node.have_data = true;
    node.invalid = true;
    node.invalid_reason = ConnectResult::BadInputs;
    node.sequence = 42;
    node.pos = {3, 1234};
    const Bytes record = encode_index(node);
    EXPECT_EQ(record.size(), 102u);

    const BlockIndex back = decode_index(record);
    EXPECT_EQ(back.hash, node.hash);
    EXPECT_EQ(back.header, node.header);
    EXPECT_EQ(back.height, 5);
    EXPECT_TRUE(back.have_data);
    EXPECT_TRUE(back.invalid);
    EXPECT_EQ(back.invalid_reason, ConnectResult::BadInputs);
    EXPECT_EQ(back.sequence, 42u);
    EXPECT_EQ(back.pos, (BlockPos{3, 1234}));

    Bytes bad_flags = record;
    bad_flags[84] = 4;  // after the 80-byte header and the height
    EXPECT_THROW(decode_index(bad_flags), DeserializeError);
}

TEST(ChainDb, MetaRoundTrip) {
    const ChainMeta meta{kChainDbVersion, test_hash("genesis"), "testnet"};
    EXPECT_EQ(decode_meta(encode_meta(meta)), meta);
    EXPECT_EQ(decode_hash(encode_hash(test_hash("tip"))), test_hash("tip"));
}
