// Milestone 12: fuzz targets for everything that decodes or parses outside input.
#include "core/address.h"
#include "core/amount.h"
#include "core/arith_uint256.h"
#include "core/block.h"
#include "core/consensus.h"
#include "core/miner.h"
#include "core/params.h"
#include "core/pow.h"
#include "core/transaction.h"
#include "crypto/sha256.h"
#include "fuzz/fuzz.h"
#include "net/addrman.h"
#include "net/banman.h"
#include "net/protocol.h"
#include "rpc/client.h"
#include "rpc/http.h"
#include "rpc/server.h"
#include "seeder/dns.h"
#include "util/base58.h"
#include "util/base64.h"
#include "util/hex.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace bound;
using namespace bound::fuzz;

std::string text_of(Data d) { return std::string(d.begin(), d.end()); }
Bytes bytes_of_string(const std::string& s) { return Bytes(s.begin(), s.end()); }

KeyPair fuzz_key() { return KeyPair::from_seed(sha256(bytes_of("BOUND fuzz key"))); }

// A signed payment from a made-up output, for seeds.
Transaction sample_payment() {
    Transaction tx;
    tx.inputs.push_back({uint256(sha256(bytes_of("a coin"))), 1});
    tx.inputs.push_back({uint256(sha256(bytes_of("another"))), 0});
    tx.outputs.push_back({150'000'000, fuzz_key().public_key()});
    tx.outputs.push_back({4'200, PublicKey{}});
    const ByteSpan memo = bytes_of("fuzz");
    tx.memo.assign(memo.begin(), memo.end());
    sign_all_inputs(tx, fuzz_key());
    return tx;
}

Block sample_block() {
    const ConsensusParams& p = regtest_params();
    Block b = genesis_block(p);
    b.header.prev_hash = p.genesis_hash;
    b.txs[0] = make_coinbase(1, 50 * kCoin, fuzz_key().public_key(), bytes_of("x"));
    b.txs.push_back(sample_payment());
    b.header.merkle_root = b.compute_merkle_root();
    return b;
}

// --- transactions and blocks -------------------------------------------------------------------

std::vector<Bytes> transaction_seeds() { return {sample_payment().serialize(), testnet_params().genesis_coinbase.serialize()}; }

FUZZ_TARGET_SEEDED(transaction, transaction_seeds) {
    Transaction tx;
    try {
        tx = Transaction::deserialize(data);
    } catch (const DeserializeError&) {
        return;
    }
    // Whatever decodes encodes back to exactly the same bytes: one encoding per transaction, so
    // its wtxid can't be varied without changing what it says.
    const Bytes again = tx.serialize();
    FUZZ_ASSERT(Bytes(data.begin(), data.end()) == again);
    FUZZ_ASSERT(Transaction::deserialize(again) == tx);
    (void)check_transaction(tx);
    (void)tx.txid();
    (void)tx.wtxid();
    for (std::size_t i = 0; i < tx.inputs.size() + 1; ++i) (void)verify_input(tx, i, fuzz_key().public_key());
}

std::vector<Bytes> block_seeds() { return {sample_block().serialize(), genesis_block(testnet_params()).serialize()}; }

FUZZ_TARGET_SEEDED(block, block_seeds) {
    Block block;
    try {
        block = Block::deserialize(data);
    } catch (const DeserializeError&) {
        return;
    }
    FUZZ_ASSERT(Bytes(data.begin(), data.end()) == block.serialize());
    (void)block.hash();
    (void)block.compute_merkle_root();
    for (const Network n : {Network::Mainnet, Network::Testnet, Network::Regtest}) (void)check_block(block, params_for(n));
}

// --- peer-to-peer messages ---------------------------------------------------------------------

// The first byte picks a message type, the rest is its payload. Whatever decodes must re-encode
// and decode to the same value.
std::vector<Bytes> payload_seeds() {
    std::vector<Bytes> seeds;
    const auto with = [&](std::uint8_t kind, const Bytes& payload) {
        Bytes b{kind};
        b.insert(b.end(), payload.begin(), payload.end());
        seeds.push_back(b);
    };
    VersionMsg v;
    v.time = 1'800'000'000;
    v.addr_you = NetAddress::ipv4(203, 0, 113, 7, 18333);
    v.listen_port = 18333;
    v.nonce = 42;
    v.user_agent = "/boundd:0.9.0/";
    v.height = 101;
    with(0, encode_version(v));
    with(1, encode_nonce(7));
    with(2, encode_getheaders({{regtest_params().genesis_hash, uint256()}, uint256()}));
    const BlockHeader h[] = {regtest_params().genesis, sample_block().header};
    with(3, encode_headers(h));
    const InvItem inv[] = {{InvType::Block, regtest_params().genesis_hash}};
    with(4, encode_inventory(inv));
    const AddrEntry addr[] = {{1'800'000'000, kNodeNetwork, NetAddress::ipv4(198, 51, 100, 1, 18333)}};
    with(5, encode_addresses(addr));
    return seeds;
}

template <class Decode, class Encode>
void round_trip(ByteSpan payload, Decode decode, Encode encode) {
    try {
        const auto value = decode(payload);
        FUZZ_ASSERT(decode(encode(value)) == value);
    } catch (const DeserializeError&) {
    } catch (const ProtocolError&) {
    }
}

FUZZ_TARGET_SEEDED(p2p_payload, payload_seeds) {
    if (data.empty()) return;
    const ByteSpan payload = data.subspan(1);
    switch (data[0] % 6) {
    case 0: round_trip(payload, decode_version, encode_version); break;
    case 1: round_trip(payload, decode_nonce, encode_nonce); break;
    case 2: round_trip(payload, decode_getheaders, encode_getheaders); break;
    case 3: round_trip(payload, decode_headers, [](const std::vector<BlockHeader>& v) { return encode_headers(v); }); break;
    case 4: round_trip(payload, decode_inventory, [](const std::vector<InvItem>& v) { return encode_inventory(v); }); break;
    default: round_trip(payload, decode_addresses, [](const std::vector<AddrEntry>& v) { return encode_addresses(v); }); break;
    }
}

std::vector<Bytes> stream_seeds() {
    const Magic& m = testnet_params().message_start;
    Bytes s = frame_message(m, command::kPing, encode_nonce(1));
    const Bytes t = frame_message(m, command::kTx, sample_payment().serialize());
    s.insert(s.end(), t.begin(), t.end());
    return {s};
}

// A peer's byte stream, cut into messages whatever the chunking.
FUZZ_TARGET_SEEDED(message_stream, stream_seeds) {
    MessageReader reader(testnet_params().message_start);
    Input in(data);
    try {
        while (!in.empty()) {
            reader.feed(in.bytes(1 + in.u8()));
            while (const std::optional<Message> m = reader.next()) (void)m;
        }
    } catch (const ProtocolError&) {
    }
}

// --- text: encodings, addresses, amounts, the console ------------------------------------------

std::vector<Bytes> text_seeds() {
    std::vector<Bytes> seeds;
    for (const Network n : {Network::Mainnet, Network::Testnet}) {
        seeds.push_back(bytes_of_string(encode_address(fuzz_key().public_key(), params_for(n).address_version)));
    }
    seeds.push_back(bytes_of_string("0123456789abcdef"));
    seeds.push_back(bytes_of_string("Zm9vYmFy"));
    seeds.push_back(bytes_of_string("20999999.97690000"));
    seeds.push_back(bytes_of_string(R"(sendtoaddress "two words" 1.5 \"x\")"));
    return seeds;
}

FUZZ_TARGET_SEEDED(text, text_seeds) {
    const std::string s = text_of(data);
    if (const std::optional<Bytes> b = from_hex(s)) FUZZ_ASSERT(from_hex(to_hex(*b)) == b);
    if (const std::optional<Bytes> b = base64_decode(s)) FUZZ_ASSERT(base64_decode(base64_encode(*b)) == b);
    if (const std::optional<Bytes> b = base58_decode(s)) FUZZ_ASSERT(base58_decode(base58_encode(*b)) == b);
    if (const std::optional<Bytes> b = base58check_decode(s)) FUZZ_ASSERT(base58check_decode(base58check_encode(*b)) == b);
    // An address has exactly one spelling: if it decodes, it's what encoding its key gives.
    for (const Network n : {Network::Mainnet, Network::Testnet, Network::Regtest}) {
        const AddressVersion& v = params_for(n).address_version;
        if (const std::optional<PublicKey> key = decode_address(s, v)) FUZZ_ASSERT(encode_address(*key, v) == s);
    }
    if (const std::optional<Amount> a = parse_coins(s)) FUZZ_ASSERT(parse_coins(format_coins(*a)) == a);
    try {
        const std::vector<std::string> words = split_words(s);
        for (std::size_t i = 1; i < words.size(); ++i) {
            try {
                (void)cli_param(words[0], i - 1, words[i]);
            } catch (const std::invalid_argument&) {
            }
        }
    } catch (const std::invalid_argument&) {
    }
}

// --- difficulty --------------------------------------------------------------------------------

FUZZ_TARGET(difficulty) {
    Input in(data);
    const std::uint32_t bits = in.u32();
    if (const std::optional<uint256> target = target_from_compact(bits)) {
        const std::uint32_t canonical = compact_from_target(*target);
        FUZZ_ASSERT(compact_from_target(target_from_compact(canonical).value()) == canonical);
        FUZZ_ASSERT(arith_uint256(target_from_compact(canonical).value()) <= arith_uint256(*target));
        (void)block_proof(bits);
    }
    // ASERT with any anchor, schedule and limit: inside [1, pow_limit], or refused.
    const ConsensusParams& p = params_for(static_cast<Network>(in.u8() % 3));
    const arith_uint256 limit(p.pow_limit);
    const std::optional<uint256> anchor_bits = target_from_compact(in.u32());
    arith_uint256 anchor = anchor_bits ? arith_uint256(*anchor_bits) : arith_uint256(1);
    if (anchor > limit) anchor = limit;
    if (anchor == 0) anchor = 1;
    const std::int64_t time_diff = in.i64() % (std::int64_t{1} << 40);
    const std::int64_t height_diff = static_cast<std::int64_t>(in.u32());
    const std::int64_t half_life = 1 + in.u32() % 1'000'000;
    try {
        const arith_uint256 t = asert_target(anchor, time_diff, height_diff, 600, half_life, limit);
        FUZZ_ASSERT(t >= 1 && t <= limit);
        // A later parent never makes the next block harder.
        if (time_diff < (std::int64_t{1} << 40) - 1000) {
            FUZZ_ASSERT(asert_target(anchor, time_diff + 1000, height_diff, 600, half_life, limit) >= t);
        }
    } catch (const std::invalid_argument&) {
    }
}

// --- saved state: peers.dat, banlist.dat -------------------------------------------------------

FUZZ_TARGET(saved_state) {
    if (data.empty()) return;
    const ByteSpan rest = data.subspan(1);
    try {
        if (data[0] % 2 == 0) {
            const AddrMan a = AddrMan::deserialize(rest, 1);
            FUZZ_ASSERT(AddrMan::deserialize(a.serialize(), 1).serialize() == a.serialize());
        } else {
            const BanMan b = BanMan::deserialize(rest);
            FUZZ_ASSERT(BanMan::deserialize(b.serialize()).serialize() == b.serialize());
        }
    } catch (const DeserializeError&) {
    }
}

// --- DNS (the seeder answers strangers over UDP) -----------------------------------------------

FUZZ_TARGET(dns) {
    const std::optional<DnsQuery> query = parse_dns_query(data);
    if (!query) return;
    DnsZone zone;
    zone.host = "seed.example.org";
    zone.nameserver = "ns.example.org";
    zone.mailbox = "hostmaster.example.org";
    const NetAddress addresses[] = {NetAddress::ipv4(198, 51, 100, 1, 18333), NetAddress::ipv4(203, 0, 113, 9, 18333)};
    const Bytes reply = build_dns_reply(*query, zone, addresses);
    FUZZ_ASSERT(reply.size() >= 12);
    (void)build_dns_error(0, 2);
}

// --- HTTP and JSON-RPC (the explorer serves strangers; the RPC server serves localhost) ---------

std::vector<Bytes> http_seeds() {
    return {bytes_of_string("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nAuthorization: Basic abc\r\n\r\nhello"),
            bytes_of_string("GET /block/0 HTTP/1.0\r\n\r\n")};
}

FUZZ_TARGET_SEEDED(http, http_seeds) {
    Input in(data);
    HttpRequestReader reader;
    try {
        while (!in.empty()) {
            reader.feed(in.bytes(1 + in.u8()));
            while (const std::optional<HttpRequest> r = reader.next()) (void)r;
        }
    } catch (const HttpError& e) {
        FUZZ_ASSERT(e.status >= 400 && e.status < 600);
    }
    try {
        (void)parse_http_response(text_of(data), data.size() % 2 == 0);
    } catch (const std::runtime_error&) {
    }
}

std::vector<Bytes> rpc_seeds() {
    return {bytes_of_string(R"({"jsonrpc":"2.0","id":1,"method":"echo","params":["a",2,true]})"),
            bytes_of_string(R"([{"id":1,"method":"add","params":[1,"2.5"]},{"id":2,"method":"nope"}])")};
}

FUZZ_TARGET_SEEDED(json_rpc, rpc_seeds) {
    static const RpcTable table = [] {
        RpcTable t;
        t.add("echo", "[x]...", "Returns its parameters.", 0, 3, [](const RpcParams& p) -> json {
            json out = json::array();
            for (std::size_t i = 0; i < 3; ++i) {
                if (p.has(i)) out.push_back(p.opt_str(i).value_or(""));
            }
            return out;
        });
        t.add("add", "<n> <amount> [flag] [rate]", "Reads every kind of parameter.", 2, 4, [](const RpcParams& p) -> json {
            json out = {{"n", p.integer(0)}, {"amount", format_coins(p.amount(1))}};
            if (p.has(2)) out["flag"] = p.boolean(2);
            if (p.has(3)) out["rate"] = p.number(3);
            return out;
        });
        return t;
    }();
    const std::string cookie = "__cookie__:0123";
    HttpRequest request;
    request.method = data.size() % 3 == 0 ? "GET" : "POST";
    request.target = "/";
    request.headers["authorization"] = "Basic " + base64_encode(bytes_of(data.size() % 5 == 0 ? std::string("x:y") : cookie));
    request.body = text_of(data);
    const HttpResponse response = serve_json_rpc(table, cookie, request);
    FUZZ_ASSERT(response.status >= 200 && response.status < 600);
    const json parsed = json::parse(request.body, nullptr, false);
    if (parsed.is_object()) (void)table.call(parsed);
}

}  // namespace
