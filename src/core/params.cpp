#include "core/params.h"

#include "core/miner.h"
#include "core/pow.h"

namespace bound {

namespace {

ConsensusParams make_testnet() {
    ConsensusParams p;
    p.network_name = "testnet";

    // Milestone 1: proof of work and time rules. Milestone 16: the proof of work is BoundHash with
    // mainnet's 512 KiB (pow_lines' default), and the easiest target still takes about 65k hashes:
    // about 15 seconds for a 6-core PC.
    p.pow_limit_bits = 0x1f00ffff;
    p.pow_limit = target_from_compact(p.pow_limit_bits).value();
    p.max_future_block_time = 2 * 60 * 60;
    p.median_time_span = 11;

    // Milestone 2: economics (1-minute blocks, 5 BOUND halving every ~4 years).
    p.target_spacing = 60;
    p.initial_subsidy = 5 * kCoin;
    p.halving_interval = 2'100'000;
    p.max_block_size = 1'000'000;
    p.address_version = 0xf0;

    // Milestone 3.
    p.coinbase_maturity = 100;

    // Milestone 4: Bitcoin's retarget rule over one day of blocks instead of two weeks.
    p.retarget_interval = 1440;

    // Milestone 6. The magic is picked like Bitcoin's: every byte has its high bit set, so it's
    // never ASCII text and never valid UTF-8, and it's unlikely to appear inside other data.
    // Milestone 16: a new magic for the new chain (it was b0 d4 e7 f5), so nodes from before
    // BoundHash and after it drop each other at the first message instead of trading headers that
    // neither can accept. Bitcoin's testnets did the same at each reset.
    p.message_start = {0xb3, 0xd8, 0xea, 0xf1};
    p.default_port = 18333;
    p.default_rpc_port = 18332;

    // Milestone 8. Seeds: the host names (or addresses) of the testnet's DNS seeders and always-on
    // nodes, e.g. "seed.example.org" (the default port is assumed; "host:port" names another).
    // A node asks them when it knows too few peers; after that, peers come from address gossip.
    // Milestone 15: the DNS seeders (seed.bound.sarl, seed2.bound.sarl) serve mainnet from launch
    // day, as a server's one DNS port can only answer for one network. Testnet's seeds are the
    // always-on nodes' own static addresses: Google Cloud (us-west1) and Microsoft Azure (South India).
    p.seeds = {
        "136.86.151.244",
        "104.211.211.39",
    };
    // The least work a chain must have before a fresh node stores its headers (docs/hardening.md).
    // Milestone 16 started the testnet over with BoundHash, so there's no chain to require yet: raise
    // it as the new one grows.
    p.minimum_chain_work = 0;

    // Genesis, mined by `boundd genesis`. It pays the all-zero key, which nobody can sign for
    // (a small-order point that libsodium rejects), and its output never enters the UTXO set.
    // Milestone 16: the same block as before (the project owner kept 01/Nov/2026), mined again
    // with BoundHash; only its nonce and hash changed.
    p.genesis_coinbase = make_coinbase(
        0, 5 * kCoin, PublicKey{},
        bytes_of("BOUND testnet 01/Nov/2026: a proof-of-work coin built from scratch in C++"));
    p.genesis.version = 1;
    p.genesis.merkle_root = genesis_block(p).compute_merkle_root();
    p.genesis.time = 1793491200;  // 2026-11-01 00:00:00 UTC
    p.genesis.bits = p.pow_limit_bits;
    p.genesis.nonce = 38222;
    p.genesis_hash = uint256::from_hex("019fd113f42b4265bd706686e723bb46282772e655334b9c024f5c960e7ae8d1").value();
    return p;
}

// Milestone 10: mainnet, with Bitcoin's schedule (the project owner's choice): 10-minute blocks,
// 50 BOUND halving every 210,000 blocks, 20,999,999.9769 BOUND in all, no premine.
ConsensusParams make_mainnet() {
    ConsensusParams p;
    p.network_name = "mainnet";
    p.launched = true;  // since launch day, 16/Nov/2026

    // Milestone 15 (the project owner's choice): difficulty 1 as Bitcoin has it, where the genesis
    // starts and the easiest the chain can ever get; ASERT makes it harder as miners join, and never
    // easier. Milestone 16 (the project owner's choice): the proof of work is BoundHash, about a
    // thousand times slower per hash than SHA-256d, so difficulty 1 is 2^22 hashes instead of
    // Bitcoin's 2^32. That keeps what Bitcoin's difficulty 1 meant in practice: one PC on every core
    // (a 6-core desktop does ~5,000 a second) finds a block in about a quarter of an hour.
    p.pow_limit_bits = 0x1e03ffff;
    p.pow_limit = target_from_compact(p.pow_limit_bits).value();
    p.max_future_block_time = 2 * 60 * 60;
    p.median_time_span = 11;

    p.target_spacing = 10 * 60;
    p.initial_subsidy = 50 * kCoin;
    p.halving_interval = 210'000;
    p.max_block_size = 1'000'000;
    p.address_version = {0x46, 0xfb};  // every address starts with 'B' (52 characters)
    p.coinbase_maturity = 100;
    // Milestone 11 (the project owner's choice): ASERT sets every block's difficulty, with Bitcoin
    // Cash's half-life of 2 days. The retarget interval below isn't used.
    p.asert_half_life = 2 * 24 * 60 * 60;
    p.retarget_interval = 2016;

    p.message_start = {0xd6, 0xb3, 0xe8, 0xf7};  // high bits set, as testnet's (see there)
    p.default_port = 7333;
    p.default_rpc_port = 7332;
    // Milestone 15: the DNS seeders first (bound-server sets them up; docs/running-a-server.md),
    // then the always-on nodes' static addresses, in case DNS fails: Google Cloud (us-west1) and
    // Microsoft Azure (South India).
    p.seeds = {
        "seed.bound.sarl",
        "seed2.bound.sarl",
        "136.86.151.244",
        "104.211.211.39",
    };
    p.minimum_chain_work = 0;  // no chain yet: each release after launch raises it

    // Launch day, 16/Nov/2026. The genesis quotes a headline from that day's front page of The
    // Indian Express (the project owner's choice), so nobody can have mined mainnet before that day,
    // as Bitcoin's genesis quotes The Times of 3 January 2009. Its reward pays a key nobody holds and
    // never enters the UTXO set: no premine. Anyone can mine it again and get the same block:
    // boundd --mainnet genesis "<the message>" 1794824661
    p.genesis_coinbase = make_coinbase(
        0, 50 * kCoin, PublicKey{},
        bytes_of("BOUND 16/Nov/2026 12 NBFCs lose over Rs 65 crore in three months to cyber fraud in Delhi-NCR"));
    p.genesis.version = 1;
    p.genesis.merkle_root = genesis_block(p).compute_merkle_root();
    p.genesis.time = 1794824661;  // 2026-11-16 10:24:21 UTC
    p.genesis.bits = p.pow_limit_bits;
    p.genesis.nonce = 651748;
    p.genesis_hash = uint256::from_hex("be44789ea517f65d9c690a4690e45ac5189777ff1d34b1ced3d73c3c20674bad").value();
    return p;
}

// Milestone 10: regtest, Bitcoin's private test network: blocks cost a couple of hashes, the
// difficulty never moves, and halvings come every 150 blocks so tests can reach them.
ConsensusParams make_regtest() {
    ConsensusParams p;
    p.network_name = "regtest";

    p.pow_limit_bits = 0x207fffff;  // half of all hashes meet it
    p.pow_limit = target_from_compact(p.pow_limit_bits).value();
    p.pow_lines = 64;  // milestone 16: BoundHash in 8 KiB, so tests mine thousands of blocks quickly
    p.pow_no_retargeting = true;
    p.max_future_block_time = 2 * 60 * 60;
    p.median_time_span = 11;

    p.target_spacing = 10 * 60;
    p.initial_subsidy = 50 * kCoin;
    p.halving_interval = 150;
    p.max_block_size = 1'000'000;
    p.address_version = 0xf0;  // testnet's, as Bitcoin's regtest shares testnet's
    p.coinbase_maturity = 100;
    p.retarget_interval = 2016;

    p.message_start = {0xc1, 0xe5, 0xa9, 0xd2};
    p.default_port = 18544;
    p.default_rpc_port = 18543;
    p.seeds = {};
    p.minimum_chain_work = 0;

    p.genesis_coinbase = make_coinbase(0, 50 * kCoin, PublicKey{}, bytes_of("BOUND regtest"));
    p.genesis.version = 1;
    p.genesis.merkle_root = genesis_block(p).compute_merkle_root();
    p.genesis.time = 1793491200;  // 2026-11-01 00:00:00 UTC
    p.genesis.bits = p.pow_limit_bits;
    p.genesis.nonce = 0;  // half of all hashes meet regtest's target, and the first try did
    p.genesis_hash = uint256::from_hex("50c65d2c8b59bc4dd295e7afa6d42c52a138df9a3b02df467db4c4693d2e0456").value();
    return p;
}

}  // namespace

const ConsensusParams& mainnet_params() {
    static const ConsensusParams params = make_mainnet();
    return params;
}

const ConsensusParams& testnet_params() {
    static const ConsensusParams params = make_testnet();
    return params;
}

const ConsensusParams& regtest_params() {
    static const ConsensusParams params = make_regtest();
    return params;
}

const ConsensusParams& params_for(Network network) {
    switch (network) {
    case Network::Mainnet: return mainnet_params();
    case Network::Testnet: return testnet_params();
    case Network::Regtest: return regtest_params();
    }
    return testnet_params();
}

std::string_view network_name(Network network) { return params_for(network).network_name; }

std::optional<Network> network_flag(std::string_view arg) {
    if (arg == "--mainnet") return Network::Mainnet;
    if (arg == "--testnet") return Network::Testnet;
    if (arg == "--regtest") return Network::Regtest;
    return std::nullopt;
}

}  // namespace bound
