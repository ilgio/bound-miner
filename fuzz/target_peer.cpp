// Milestone 12: the node's widest attack surface, a stranger's messages. The input is a script
// for one peer: messages of any type with fuzzed payloads (framed correctly, so the fuzzer needn't
// find checksums), a well-formed handshake, blocks built on the spot and then varied, raw bytes,
// and the clock moving on. On regtest any hash meets the target half the time, so fuzzed blocks
// and headers can be valid and reach deep into validation.
//
// PeerManager::receive catches the errors bad input is meant to raise (DeserializeError,
// ProtocolError) and returns false to drop the peer. Anything else escaping it would take the
// whole node down, so here it fails the run.
#include "core/mempool.h"
#include "core/miner.h"
#include "core/params.h"
#include "core/validation.h"
#include "crypto/sha256.h"
#include "fuzz/fuzz.h"
#include "net/addrman.h"
#include "net/banman.h"
#include "net/peerman.h"
#include "net/protocol.h"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <vector>

namespace {

using namespace bound;
using namespace bound::fuzz;

constexpr std::string_view kCommands[] = {
    command::kVersion, command::kVerack, command::kPing,     command::kPong,    command::kGetHeaders,
    command::kHeaders, command::kGetData, command::kBlock,   command::kNotFound, command::kGetAddr,
    command::kAddr,    command::kInv,     command::kTx,
};

// Steps a seed can be written in.
enum Op : std::uint8_t { kMessage = 0, kHandshake = 4, kNewBlock = 5, kClock = 6, kRaw = 7 };

std::vector<Bytes> peer_seeds() {
    const auto u16 = [](Bytes& b, std::uint16_t v) {
        b.push_back(static_cast<std::uint8_t>(v));
        b.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    std::vector<Bytes> seeds;
    // Inbound; handshake; a new block; a getheaders from genesis; the clock; a ping.
    Bytes s{1, kHandshake};
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<std::uint8_t>(i));  // nonce
    s.insert(s.end(), {3, 0x10, 0x27, 7, 0, 0});                             // height, port, addr byte, agent length
    s.insert(s.end(), {kNewBlock, 0, 0, 0, 0});                              // and its four choices
    s.push_back(kMessage);
    u16(s, 4);  // getheaders
    const Bytes gh = encode_getheaders({{regtest_params().genesis_hash}, uint256()});
    u16(s, static_cast<std::uint16_t>(gh.size()));
    s.insert(s.end(), gh.begin(), gh.end());
    s.push_back(kClock);
    u16(s, 600);
    s.push_back(kMessage);
    u16(s, 2);  // ping
    const Bytes nonce = encode_nonce(9);
    u16(s, static_cast<std::uint16_t>(nonce.size()));
    s.insert(s.end(), nonce.begin(), nonce.end());
    seeds.push_back(s);
    // Outbound, straight into headers and inv before any handshake.
    Bytes o{0, kMessage};
    u16(o, 5);
    const BlockHeader h[] = {regtest_params().genesis};
    const Bytes hs = encode_headers(h);
    u16(o, static_cast<std::uint16_t>(hs.size()));
    o.insert(o.end(), hs.begin(), hs.end());
    seeds.push_back(o);
    return seeds;
}

FUZZ_TARGET_SEEDED(peer, peer_seeds) {
    const ConsensusParams& p = regtest_params();
    const Magic& magic = p.message_start;
    const PublicKey miner = KeyPair::from_seed(sha256(bytes_of("BOUND fuzz miner"))).public_key();

    // A short chain to talk about.
    Chainstate chain(p);
    std::uint32_t time = p.genesis.time;
    for (int i = 0; i < 3; ++i) {
        time += 600;
        (void)chain.accept_block(mine_block(make_block_template(chain, miner, {}, time).value(), p), time + 600);
    }
    Mempool mempool(chain);
    AddrMan addrman(1);
    BanMan banman;
    PeerConfig config;
    config.listen_port = p.default_port;
    PeerManager peers(chain, mempool, addrman, banman, config, 1);

    Input in(data);
    std::int64_t now = std::int64_t{time} + 600;
    const PeerId id = 1;
    peers.add_peer(id, NetAddress::ipv4(198, 51, 100, 7, p.default_port), in.flag(), now);
    const auto drain = [&] {
        const ByteSpan out = peers.pending(id);
        if (!out.empty()) peers.sent(id, out.size());
    };
    drain();

    for (int step = 0; step < 64 && !in.empty(); ++step) {
        bool keep = true;
        switch (in.u8() % 8) {
        case 0:
        case 1:
        case 2:
        case 3: {  // any message, fuzzed payload
            const std::string_view cmd = kCommands[in.pick(std::size(kCommands))];
            keep = peers.receive(id, frame_message(magic, cmd, in.bytes(4096)), now);
            break;
        }
        case kHandshake: {  // a well-formed version, then verack
            VersionMsg v;
            v.time = now;
            v.nonce = in.u64();
            v.height = in.u8();
            v.listen_port = in.u16();
            v.addr_you = NetAddress::ipv4(203, 0, 113, in.u8(), p.default_port);
            v.user_agent = in.text(32);
            keep = peers.receive(id, frame_message(magic, command::kVersion, encode_version(v)), now) &&
                   peers.receive(id, frame_message(magic, command::kVerack, {}), now);
            break;
        }
        case kNewBlock: {  // a block on the tip, possibly varied after the fact
            Block b = make_block_template(chain, miner, {}, static_cast<std::uint32_t>(now)).value();
            if (in.flag()) b.txs[0].memo = in.bytes(16);
            b.header.merkle_root = b.compute_merkle_root();
            if (in.flag()) b.header.time += in.u16();
            b = mine_block(std::move(b), p);
            if (in.flag()) b.header.prev_hash = uint256(sha256(in.bytes(8)));  // somewhere unknown
            if (in.flag()) b.txs.push_back(b.txs[0]);                           // a duplicate
            keep = peers.receive(id, frame_message(magic, command::kBlock, b.serialize()), now);
            break;
        }
        case kClock: {  // time passes; the peer may be dropped for being slow
            now += in.u16();
            const std::vector<PeerId> dropped = peers.tick(now);
            keep = std::find(dropped.begin(), dropped.end(), id) == dropped.end();
            break;
        }
        default:  // bytes with no framing at all
            keep = peers.receive(id, in.bytes(256), now);
            break;
        }
        drain();
        if (!keep) break;
    }
    peers.remove_peer(id);
}

}  // namespace
