#include "core/leveldb_store.h"
#include "core/miner.h"
#include "core/storage.h"
#include "core/validation.h"
#include "core/wallet.h"
#include "node/node.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bound;

namespace {

// Milestone 5: a chainstate over storage that outlives it. Destroying the Chainstate and opening
// a new one on the same storage is a restart.
class PersistenceTest : public ::testing::Test {
protected:
    // Mines and accepts `count` blocks for `miner` on `parent`, a minute per height after the test
    // genesis; returns the last one.
    static const BlockIndex& extend(Chainstate& chain, const BlockIndex& parent, const KeyPair& miner, int count = 1) {
        const BlockIndex* node = &parent;
        for (int i = 0; i < count; ++i) {
            const auto time = static_cast<std::uint32_t>(kTestGenesisTime + 60 * (node->height + 1));
            const Block block = block_on(*node, chain.params(), miner.public_key(), time);
            if (!chain.accept_block(block, kTestNow).accepted()) throw std::logic_error("test block rejected");
            node = chain.find(block.hash());
        }
        return *node;
    }

    // A block on `parent` whose coinbase claims one unit too many: it fails only once connected.
    Block greedy_on(const BlockIndex& parent, const KeyPair& miner) const {
        Block block = block_on(parent, params, miner.public_key(), kTestGenesisTime + 60 * (parent.height + 1));
        block.txs[0].outputs[0].amount += 1;
        block.header.merkle_root = block.compute_merkle_root();
        block.header = mine(block.header, params).value();
        return block;
    }

    const ConsensusParams params = easy_params();
    MemoryStore db;
    MemoryBlockStore blocks;
    const KeyPair alice = test_key("alice");
    const KeyPair bob = test_key("bob");
    const KeyPair carol = test_key("carol");
};

}  // namespace

TEST_F(PersistenceTest, RestartKeepsEverything) {
    uint256 tip;
    arith_uint256 work;
    UtxoSet utxos;
    std::size_t tree_size = 0;
    uint256 side;
    uint256 bad;
    Block waiting;  // its header is stored, its transactions aren't yet
    {
        Chainstate chain(params, db, blocks);
        mine_blocks(chain, alice.public_key(), 101);
        const std::optional<Transaction> pay =
            create_payment(chain.utxos(), alice, bob.public_key(), 2 * kCoin, kCoin / 100, 102, params);
        ASSERT_TRUE(pay.has_value());
        ASSERT_TRUE(chain.accept_block(next_block(chain, alice.public_key(), {*pay}), kTestNow).accepted());

        side = extend(chain, chain.at(100), carol).hash;  // a side branch
        const Block greedy = greedy_on(chain.tip(), carol);
        ASSERT_EQ(chain.accept_block(greedy, kTestNow).body, ConnectResult::CoinbaseTooLarge);
        bad = greedy.hash();
        waiting = block_on(chain.tip(), params, bob.public_key(), kTestGenesisTime + 60 * 103);
        ASSERT_EQ(chain.accept_header(waiting.header, kTestNow), AcceptResult::Accepted);

        tip = chain.tip().hash;
        work = chain.tip().chain_work;
        utxos = chain.utxos();
        tree_size = chain.tree_size();
    }

    Chainstate chain(params, db, blocks);
    EXPECT_EQ(chain.height(), 102);
    EXPECT_EQ(chain.tip().hash, tip);
    EXPECT_EQ(chain.tip().chain_work, work);
    EXPECT_TRUE(chain.utxos() == utxos);
    EXPECT_EQ(balance(chain.utxos(), bob.public_key()), 2 * kCoin);
    EXPECT_EQ(chain.tree_size(), tree_size);
    EXPECT_EQ(chain.block_at(102).txs.size(), 2u);

    ASSERT_NE(chain.find(side), nullptr);
    EXPECT_TRUE(chain.find(side)->have_data);
    EXPECT_FALSE(chain.in_active_chain(*chain.find(side)));
    ASSERT_NE(chain.find(bad), nullptr);
    EXPECT_TRUE(chain.find(bad)->invalid);
    EXPECT_EQ(chain.find(bad)->invalid_reason, ConnectResult::CoinbaseTooLarge);
    ASSERT_NE(chain.find(waiting.hash()), nullptr);
    EXPECT_FALSE(chain.find(waiting.hash())->have_data);

    // And it carries on from there: the waiting header's block connects.
    EXPECT_TRUE(chain.accept_block(waiting, kTestNow).accepted());
    EXPECT_EQ(chain.height(), 103);
}

// The order chains became complete is stored too, so of two with equal work the first one seen
// stays the tip after a restart.
TEST_F(PersistenceTest, EqualWorkTieSurvivesARestart) {
    uint256 tip;
    {
        Chainstate chain(params, db, blocks);
        const BlockIndex& genesis = chain.tip();
        tip = extend(chain, genesis, alice, 2).hash;
        extend(chain, genesis, carol, 2);
        ASSERT_EQ(chain.tip().hash, tip);
    }
    const Chainstate chain(params, db, blocks);
    EXPECT_EQ(chain.tip().hash, tip);
}

// Undo data is stored, so a reorg after a restart can still take back a payment.
TEST_F(PersistenceTest, ReorgAfterARestart) {
    {
        Chainstate chain(params, db, blocks);
        mine_blocks(chain, alice.public_key(), 101);
        const std::optional<Transaction> pay =
            create_payment(chain.utxos(), alice, bob.public_key(), 7 * kCoin, kCoin / 1000, 102, params);
        ASSERT_TRUE(pay.has_value());
        ASSERT_TRUE(chain.accept_block(next_block(chain, alice.public_key(), {*pay}), kTestNow).accepted());
    }
    Chainstate chain(params, db, blocks);
    const BlockIndex& carols_tip = extend(chain, chain.at(101), carol, 2);
    ASSERT_EQ(&chain.tip(), &carols_tip);
    EXPECT_EQ(balance(chain.utxos(), bob.public_key()), 0);

    Chainstate direct(params);
    for (int h = 1; h <= chain.height(); ++h) ASSERT_TRUE(direct.accept_block(chain.block_at(h), kTestNow).accepted());
    EXPECT_TRUE(chain.utxos() == direct.utxos());
}

TEST_F(PersistenceTest, RefusesAnotherNetworksChain) {
    { const Chainstate chain(params, db, blocks); }
    ConsensusParams other = params;
    const ByteSpan memo = bytes_of("another network");
    other.genesis_coinbase.memo.assign(memo.begin(), memo.end());
    other.genesis.merkle_root = genesis_block(other).compute_merkle_root();
    other.genesis = mine(other.genesis, other).value();
    other.genesis_hash = other.genesis.hash();
    // Milestone 15: the same network's name with another genesis is a chain from before launch.
    try {
        const Chainstate chain(other, db, blocks);
        ADD_FAILURE() << "opened a chain with another genesis";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("another genesis block"), std::string::npos) << e.what();
    }
    other.network_name = "elsewhere";
    try {
        const Chainstate chain(other, db, blocks);
        ADD_FAILURE() << "opened another network's chain";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("another network"), std::string::npos) << e.what();
    }
}

namespace {

struct Crash : std::runtime_error {
    Crash() : std::runtime_error("simulated crash") {}
};

// Pulls the plug: lets `allowed` writes through (to either store), then throws Crash instead of
// doing the next one, so that write never happens.
class PowerSwitch {
public:
    explicit PowerSwitch(int allowed) : left_(allowed) {}
    void before_write() {
        if (left_ == 0) {
            cut_ = true;
            throw Crash();
        }
        --left_;
    }
    bool cut() const { return cut_; }

private:
    int left_;
    bool cut_ = false;
};

class CrashingStore final : public KeyValueStore {
public:
    CrashingStore(KeyValueStore& inner, PowerSwitch& power) : inner_(inner), power_(power) {}
    std::optional<Bytes> get(ByteSpan key) const override { return inner_.get(key); }
    void write(const WriteBatch& batch) override {
        power_.before_write();
        inner_.write(batch);
    }
    void scan(ByteSpan prefix, bool reverse, const std::function<bool(ByteSpan, ByteSpan)>& f) const override {
        inner_.scan(prefix, reverse, f);
    }

private:
    KeyValueStore& inner_;
    PowerSwitch& power_;
};

class CrashingBlockStore final : public BlockStore {
public:
    CrashingBlockStore(BlockStore& inner, PowerSwitch& power) : inner_(inner), power_(power) {}
    BlockPos append(const Block& block) override {
        power_.before_write();
        return inner_.append(block);
    }
    Block read(BlockPos pos) const override { return inner_.read(pos); }

private:
    BlockStore& inner_;
    PowerSwitch& power_;
};

}  // namespace

// A node with Alice's 3 blocks receives Carol's 4, and the 4th makes it reorganise. The power is
// cut after every possible number of writes in turn. Each time, the restarted node, sent Carol's
// blocks again (as a peer would resend what was lost), must end exactly where an uninterrupted
// node does.
TEST(CrashTest, AnyWriteCanBeTheLast) {
    const ConsensusParams params = easy_params();
    const PublicKey alice = test_key("alice").public_key();
    std::vector<Block> branch;
    UtxoSet expected;
    {
        Chainstate carols(params);
        for (int i = 0; i < 4; ++i) {
            branch.push_back(next_block(carols, test_key("carol").public_key()));
            ASSERT_TRUE(carols.accept_block(branch.back(), kTestNow).accepted());
        }
        expected = carols.utxos();
    }

    int crashes = 0;
    for (int allowed = 0;; ++allowed) {
        MemoryStore db;
        MemoryBlockStore blocks;
        {
            Chainstate chain(params, db, blocks);
            mine_blocks(chain, alice, 3);
        }
        PowerSwitch power(allowed);
        try {
            CrashingStore crashing_db(db, power);
            CrashingBlockStore crashing_blocks(blocks, power);
            Chainstate chain(params, crashing_db, crashing_blocks);
            for (const Block& b : branch) chain.accept_block(b, kTestNow);
        } catch (const Crash&) {
            ++crashes;
        }

        Chainstate chain(params, db, blocks);
        for (const Block& b : branch) chain.accept_block(b, kTestNow);
        EXPECT_EQ(chain.tip().hash, branch.back().hash()) << "power cut after " << allowed << " writes";
        EXPECT_TRUE(chain.utxos() == expected) << "power cut after " << allowed << " writes";
        if (!power.cut()) break;  // the whole run fit in `allowed` writes: every cut point is covered
    }
    EXPECT_GE(crashes, 10);  // 4 blocks stored, 3 disconnected, 4 connected
}

TEST(CrashTest, CrashWhileCreating) {
    const ConsensusParams params = easy_params();
    for (int allowed = 0; allowed < 2; ++allowed) {  // before storing genesis, before the first database write
        MemoryStore db;
        MemoryBlockStore blocks;
        PowerSwitch power(allowed);
        {
            CrashingStore crashing_db(db, power);
            CrashingBlockStore crashing_blocks(blocks, power);
            EXPECT_THROW({ const Chainstate chain(params, crashing_db, crashing_blocks); }, Crash);
        }
        const Chainstate chain(params, db, blocks);
        EXPECT_EQ(chain.height(), 0);
        EXPECT_EQ(chain.tip().hash, params.genesis_hash);
    }
}

// The same on the real thing: LevelDB and block files in a directory.
TEST(DiskPersistence, RestartOnDisk) {
    const ConsensusParams params = easy_params();
    const PublicKey alice = test_key("alice").public_key();
    TempDir dir;
    uint256 tip;
    {
        LevelDbStore db(dir.path() / "chainstate");
        FlatFileBlockStore blocks(dir.path() / "blocks");
        Chainstate chain(params, db, blocks);
        mine_blocks(chain, alice, 5);
        tip = chain.tip().hash;
    }
    LevelDbStore db(dir.path() / "chainstate");
    FlatFileBlockStore blocks(dir.path() / "blocks");
    Chainstate chain(params, db, blocks);
    EXPECT_EQ(chain.tip().hash, tip);
    EXPECT_EQ(chain.utxos().size(), 5u);
    mine_blocks(chain, alice, 1);
    EXPECT_EQ(chain.height(), 6);
}

TEST(DiskPersistence, DamagedBlockFilesAreCaughtAtStartup) {
    const ConsensusParams params = easy_params();
    TempDir dir;
    BlockPos tip;
    {
        LevelDbStore db(dir.path() / "chainstate");
        FlatFileBlockStore blocks(dir.path() / "blocks");
        Chainstate chain(params, db, blocks);
        mine_blocks(chain, test_key("alice").public_key(), 3);
        tip = chain.tip().pos;
    }
    {
        // Change a byte of the tip block's timestamp: 8 bytes of record header, then the time
        // sits 68 bytes into the block header.
        std::fstream f(dir.path() / "blocks" / "blk00000.dat", std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(tip.offset + 8 + 68);
        f.put(static_cast<char>(0x7f));
    }
    LevelDbStore db(dir.path() / "chainstate");
    FlatFileBlockStore blocks(dir.path() / "blocks");
    EXPECT_THROW({ const Chainstate chain(params, db, blocks); }, std::runtime_error);
}

// Milestone 16: a chain whose genesis this version replaced (the testnet starting over with
// BoundHash) is set aside with its wallet, and a new chain starts. Another network's chain, or this
// genesis's own, stays where it is.
TEST(SetAsideReplacedChain, MovesItAndItsWallet) {
    namespace fs = std::filesystem;
    const TempDir dir;
    const ConsensusParams old_params = easy_params();
    {
        DataDir data(dir.path());
        Chainstate chain(old_params, data.db, data.blocks);
        mine_blocks(chain, test_key("miner").public_key(), 3);
    }
    fs::create_directories(dir.path() / "wallet");
    std::ofstream(dir.path() / "wallet" / "marker") << "the old wallet";

    EXPECT_EQ(set_aside_replaced_chain(dir.path(), old_params), std::nullopt);  // its own genesis
    ConsensusParams elsewhere = old_params;
    elsewhere.network_name = "elsewhere";
    EXPECT_EQ(set_aside_replaced_chain(dir.path(), elsewhere), std::nullopt);  // not this network's chain
    EXPECT_TRUE(fs::exists(dir.path() / "chainstate"));

    ConsensusParams replaced = old_params;
    const ByteSpan memo = bytes_of("the same network, started over");
    replaced.genesis_coinbase.memo.assign(memo.begin(), memo.end());
    replaced.genesis.merkle_root = genesis_block(replaced).compute_merkle_root();
    replaced.genesis = mine(replaced.genesis, replaced).value();
    replaced.genesis_hash = replaced.genesis.hash();
    const std::optional<fs::path> moved = set_aside_replaced_chain(dir.path(), replaced);
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(*moved, dir.path() / ("old-chain-" + old_params.genesis_hash.to_hex()));
    for (const char* name : {"chainstate", "blocks", "wallet"}) EXPECT_FALSE(fs::exists(dir.path() / name)) << name;
    EXPECT_TRUE(fs::exists(*moved / "wallet" / "marker"));

    {
        DataDir data(dir.path());  // the new chain, from its own genesis
        const Chainstate chain(replaced, data.db, data.blocks);
        EXPECT_EQ(chain.height(), 0);
    }
    {
        DataDir data(*moved);  // the old one, intact where it was put
        const Chainstate chain(old_params, data.db, data.blocks);
        EXPECT_EQ(chain.height(), 3);
    }
    EXPECT_EQ(set_aside_replaced_chain(dir.path(), replaced), std::nullopt);  // nothing more to move
}
