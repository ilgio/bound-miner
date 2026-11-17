// Milestone 10: mainnet, testnet and regtest side by side.
#include "app/datadir.h"
#include "core/address.h"
#include "core/consensus.h"
#include "core/miner.h"
#include "core/params.h"
#include "core/pow.h"
#include "core/validation.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace bound;

namespace {

const Network kAll[] = {Network::Mainnet, Network::Testnet, Network::Regtest};

Amount total_supply(const ConsensusParams& p) {
    Amount total = 0;
    for (int halving = 0; halving < 64; ++halving) total += block_subsidy(halving * p.halving_interval, p) * p.halving_interval;
    return total;
}

}  // namespace

TEST(Networks, EveryGenesisIsValid) {
    for (const Network n : kAll) {
        const ConsensusParams& p = params_for(n);
        SCOPED_TRACE(std::string(p.network_name));
        EXPECT_EQ(target_from_compact(p.pow_limit_bits), p.pow_limit);
        EXPECT_EQ(compact_from_target(p.pow_limit), p.pow_limit_bits);
        EXPECT_TRUE(p.genesis.prev_hash.is_zero());
        EXPECT_EQ(p.genesis.bits, p.pow_limit_bits);
        EXPECT_EQ(p.genesis.hash(), p.genesis_hash);
        EXPECT_TRUE(check_proof_of_work(p.genesis, p));
        const Block g = genesis_block(p);
        EXPECT_EQ(g.compute_merkle_root(), p.genesis.merkle_root);
        EXPECT_EQ(check_block(g, p), BlockCheck::Ok);
        EXPECT_EQ(g.txs[0].outputs[0].amount, block_subsidy(0, p));
        const Chainstate chain(p);  // a chain starts from it, and its reward is never spendable
        EXPECT_EQ(chain.height(), 0);
        EXPECT_EQ(chain.utxos().size(), 0u);
    }
}

// The networks can't be mistaken for one another: on the wire, on the ports, or by genesis.
TEST(Networks, AreKeptApart) {
    std::set<std::vector<std::uint8_t>> magics;
    std::set<std::uint16_t> ports;
    std::set<std::string> geneses;
    std::set<std::string_view> names;
    for (const Network n : kAll) {
        const ConsensusParams& p = params_for(n);
        magics.insert({p.message_start.begin(), p.message_start.end()});
        ports.insert(p.default_port);
        ports.insert(p.default_rpc_port);
        geneses.insert(p.genesis_hash.to_hex());
        names.insert(p.network_name);
        for (const std::uint8_t b : p.message_start) EXPECT_GE(b, 0x80);  // never ASCII
        EXPECT_EQ(network_name(n), p.network_name);
    }
    EXPECT_EQ(magics.size(), 3u);
    EXPECT_EQ(ports.size(), 6u);
    EXPECT_EQ(geneses.size(), 3u);
    EXPECT_EQ(names, (std::set<std::string_view>{"mainnet", "testnet", "regtest"}));
}

// The project owner's choice for mainnet: Bitcoin's exact schedule, no premine.
TEST(Networks, MainnetHasBitcoinsSchedule) {
    const ConsensusParams& p = mainnet_params();
    EXPECT_EQ(p.target_spacing, 600);
    EXPECT_EQ(p.initial_subsidy, 50 * kCoin);
    EXPECT_EQ(p.halving_interval, 210'000);
    EXPECT_EQ(block_subsidy(209'999, p), 50 * kCoin);
    EXPECT_EQ(block_subsidy(210'000, p), 25 * kCoin);
    EXPECT_EQ(block_subsidy(64 * 210'000, p), 0);
    EXPECT_EQ(total_supply(p), Amount{2'099'999'997'690'000});  // 20,999,999.9769 BOUND, like Bitcoin
    EXPECT_EQ(p.coinbase_maturity, 100);
    EXPECT_EQ(p.max_block_size, 1'000'000u);
    EXPECT_EQ(p.max_future_block_time, 2 * 60 * 60);
    // Milestone 15: difficulty 1, where the chain starts and the easiest it can get. Milestone 16:
    // with BoundHash that's 2^22 hashes a block (Bitcoin's 2^32 SHA-256d hashes took a PC about as long).
    EXPECT_EQ(p.pow_limit_bits, 0x1e03ffffu);
    EXPECT_EQ(p.genesis.bits, p.pow_limit_bits);
    EXPECT_DOUBLE_EQ(difficulty(p.genesis.bits, p), 1.0);
    EXPECT_GE(block_proof(p.pow_limit_bits), arith_uint256(1) << 22);
    EXPECT_LT(block_proof(p.pow_limit_bits), (arith_uint256(1) << 22) + 100);
    EXPECT_EQ(p.pow_lines, kBoundHashLines);
    EXPECT_TRUE(p.launched);  // since launch day
    // Launch day: the genesis quotes that day's headline and is dated that day (16/Nov/2026, UTC).
    EXPECT_GE(p.genesis.time, 1794787200u);
    EXPECT_LT(p.genesis.time, 1794787200u + 24 * 60 * 60);
    EXPECT_EQ(p.genesis_coinbase.outputs.at(0).amount, 50 * kCoin);
    EXPECT_TRUE(testnet_params().launched);
    EXPECT_EQ(total_supply(testnet_params()), Amount{2'099'999'972'700'000});  // unchanged
}

// Mainnet addresses start with 'B', whatever the key: the 2-byte version fixes it.
TEST(Networks, MainnetAddressesStartWithB) {
    const ConsensusParams& main = mainnet_params();
    PublicKey low{};
    PublicKey high{};
    high.fill(0xff);
    std::vector<PublicKey> keys{low, high};
    for (const char* name : {"alice", "bob", "carol", "dave", "erin"}) keys.push_back(test_key(name).public_key());
    for (const PublicKey& key : keys) {
        const std::string a = encode_address(key, main.address_version);
        EXPECT_EQ(a.size(), 52u) << a;
        EXPECT_EQ(a[0], 'B') << a;
        EXPECT_EQ(decode_address(a, main.address_version), key);
        const std::string t = encode_address(key, testnet_params().address_version);
        EXPECT_EQ(t.size(), 51u) << t;
        EXPECT_EQ(t[0], '9') << t;
        // A mainnet address isn't a testnet one, nor the other way round.
        EXPECT_EQ(decode_address(a, testnet_params().address_version), std::nullopt);
        EXPECT_EQ(decode_address(t, main.address_version), std::nullopt);
    }
    // A one-byte version that happens to match the first byte of a two-byte one isn't enough.
    EXPECT_EQ(decode_address(encode_address(low, AddressVersion(0x46)), main.address_version), std::nullopt);
    EXPECT_EQ(decode_address(encode_address(low, main.address_version), AddressVersion(0x46)), std::nullopt);
}

// Regtest: any CPU finds a block at once, the difficulty never moves, and halvings come early.
TEST(Networks, RegtestNeverRetargets) {
    const ConsensusParams& p = regtest_params();
    EXPECT_TRUE(p.pow_no_retargeting);
    EXPECT_EQ(block_subsidy(150, p), 25 * kCoin);
    Chainstate chain(p);
    const PublicKey miner = test_key("miner").public_key();
    for (int i = 0; i < 5; ++i) {
        std::optional<Block> tmpl = make_block_template(chain, miner, {}, p.genesis.time + 600 * (i + 1));
        ASSERT_TRUE(tmpl.has_value());
        const Block block = mine_block(std::move(*tmpl), p);
        ASSERT_TRUE(chain.accept_block(block, p.genesis.time + 100'000).accepted());
    }
    EXPECT_EQ(chain.height(), 5);
    // Even where a retarget would fall, and however fast the blocks came.
    EXPECT_EQ(next_work_required(p.retarget_interval - 1, p.pow_limit_bits, 0, [](int) { return std::int64_t{0}; }, p), p.pow_limit_bits);
}

TEST(Networks, ChosenOnTheCommandLine) {
    EXPECT_EQ(network_flag("--mainnet"), Network::Mainnet);
    EXPECT_EQ(network_flag("--testnet"), Network::Testnet);
    EXPECT_EQ(network_flag("--regtest"), Network::Regtest);
    EXPECT_EQ(network_flag("--main"), std::nullopt);
    EXPECT_EQ(kDefaultNetwork, Network::Mainnet);  // since launch day

    std::vector<std::string_view> args{"--datadir", "x", "--regtest", "node", "--index"};
    EXPECT_EQ(take_network(args), Network::Regtest);
    EXPECT_EQ(args, (std::vector<std::string_view>{"--datadir", "x", "node", "--index"}));
    std::vector<std::string_view> none{"node"};
    EXPECT_EQ(take_network(none), Network::Mainnet);
    std::vector<std::string_view> same{"--mainnet", "--mainnet", "info"};
    EXPECT_EQ(take_network(same), Network::Mainnet);
    EXPECT_EQ(same, (std::vector<std::string_view>{"info"}));
    std::vector<std::string_view> both{"--mainnet", "--testnet", "info"};
    EXPECT_THROW(take_network(both), std::invalid_argument);
    // Only among the first words when told so: bound-cli's parameters are left alone.
    std::vector<std::string_view> cli{"--mainnet", "echo", "--regtest"};
    EXPECT_EQ(take_network(cli, 1), Network::Mainnet);
    EXPECT_EQ(cli, (std::vector<std::string_view>{"echo", "--regtest"}));

    for (const Network n : kAll) {
        EXPECT_EQ(default_datadir(n).filename().string(), std::string(network_name(n)));
    }
    EXPECT_NE(default_datadir(Network::Mainnet), default_datadir(Network::Testnet));
}
