#include "net/protocol.h"
#include "test_util.h"
#include "util/hex.h"
#include "util/serialize.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace bound;

namespace {

constexpr Magic kBitcoinMagic{0xf9, 0xbe, 0xb4, 0xd9};

Magic test_magic() { return easy_params().message_start; }

// True if the reader throws ProtocolError on this input.
bool rejects(const Bytes& stream) {
    MessageReader reader(test_magic());
    reader.feed(stream);
    try {
        while (reader.next()) {
        }
        return false;
    } catch (const ProtocolError&) {
        return true;
    }
}

}  // namespace

// The framing is Bitcoin's, so Bitcoin's own verack comes out byte for byte.
TEST(Framing, MatchesBitcoinsVerack) {
    EXPECT_EQ(to_hex(frame_message(kBitcoinMagic, "verack", {})),
              "f9beb4d9"                  // magic
              "76657261636b000000000000"  // "verack", NUL-padded to 12 bytes
              "00000000"                  // empty payload
              "5df6e0e2");                // sha256d("")[0..4]
}

TEST(Framing, ReaderCutsAStreamIntoMessages) {
    Bytes stream = frame_message(test_magic(), command::kPing, encode_nonce(7));
    const Bytes second = frame_message(test_magic(), command::kVerack, {});
    stream.insert(stream.end(), second.begin(), second.end());

    MessageReader reader(test_magic());
    std::vector<Message> got;
    for (const std::uint8_t byte : stream) {  // one byte at a time, the worst TCP can do
        reader.feed(ByteSpan(&byte, 1));
        while (std::optional<Message> m = reader.next()) got.push_back(*m);
    }
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].command, "ping");
    EXPECT_EQ(decode_nonce(got[0].payload), 7u);
    EXPECT_EQ(got[1].command, "verack");
    EXPECT_TRUE(got[1].payload.empty());
}

TEST(Framing, RejectsBrokenFrames) {
    const Bytes good = frame_message(test_magic(), command::kPing, encode_nonce(7));
    EXPECT_FALSE(rejects(good));

    Bytes other_network = good;
    other_network[0] ^= 1;
    EXPECT_TRUE(rejects(other_network));

    Bytes bad_checksum = good;
    bad_checksum.back() ^= 1;  // a payload byte, so the checksum no longer matches
    EXPECT_TRUE(rejects(bad_checksum));

    Bytes junk_in_command = good;
    junk_in_command[4 + 6] = 'x';  // "ping\0\0x...": text after the padding started
    EXPECT_TRUE(rejects(junk_in_command));

    Bytes empty_command = good;
    std::fill(empty_command.begin() + 4, empty_command.begin() + 16, std::uint8_t{0});
    EXPECT_TRUE(rejects(empty_command));

    // Refused from the header alone, before 4 MB arrive.
    Bytes huge = good;
    const std::uint32_t length = kMaxMessageSize + 1;
    for (int i = 0; i < 4; ++i) huge[16 + i] = static_cast<std::uint8_t>(length >> (8 * i));
    huge.resize(kMessageHeaderSize);
    EXPECT_TRUE(rejects(huge));
}

TEST(Messages, Version) {
    VersionMsg v;
    v.time = 1'800'000'000;
    v.addr_you = NetAddress::ipv4(1, 2, 3, 4, 18333);
    v.listen_port = 18333;
    v.nonce = 0x0123456789abcdefULL;
    v.user_agent = "/boundd:0.6.0/";
    v.height = 42;
    const Bytes encoded = encode_version(v);
    EXPECT_EQ(encoded.size(), 4 + 8 + 8 + 18 + 2 + 8 + 1 + v.user_agent.size() + 4);
    EXPECT_EQ(decode_version(encoded), v);

    Bytes long_agent = encode_version(VersionMsg{.user_agent = std::string(kMaxUserAgent + 1, 'a')});
    EXPECT_THROW(decode_version(long_agent), DeserializeError);
}

TEST(Messages, NonceAndTrailingBytes) {
    EXPECT_EQ(decode_nonce(encode_nonce(99)), 99u);
    Bytes longer = encode_nonce(99);
    longer.push_back(0);
    EXPECT_THROW(decode_nonce(longer), DeserializeError);
}

TEST(Messages, GetHeaders) {
    const GetHeadersMsg m{{test_hash("a"), test_hash("b")}, test_hash("stop")};
    EXPECT_EQ(decode_getheaders(encode_getheaders(m)), m);
    GetHeadersMsg too_long;
    too_long.locator.assign(kMaxLocator + 1, test_hash("x"));
    EXPECT_THROW(decode_getheaders(encode_getheaders(too_long)), DeserializeError);
}

TEST(Messages, Headers) {
    const std::vector<BlockHeader> headers{easy_params().genesis, testnet_params().genesis};
    EXPECT_EQ(decode_headers(encode_headers(headers)), headers);
    EXPECT_EQ(encode_headers(headers).size(), 1 + 2 * BlockHeader::kSize);
    const std::vector<BlockHeader> too_many(kMaxHeaders + 1, easy_params().genesis);
    EXPECT_THROW(decode_headers(encode_headers(too_many)), DeserializeError);
}

TEST(Messages, Inventory) {
    const std::vector<InvItem> items{{InvType::Block, test_hash("a")}, {InvType::Block, test_hash("b")}};
    EXPECT_EQ(decode_inventory(encode_inventory(items)), items);
    Bytes lying = encode_inventory(items);
    lying[0] = 3;  // claims an item that isn't there
    EXPECT_THROW(decode_inventory(lying), DeserializeError);
}

TEST(Messages, AddressesKeepBitcoinsBigEndianPort) {
    const AddrEntry entry{1, kNodeNetwork, NetAddress::ipv4(1, 2, 3, 4, 18333)};
    const std::vector<AddrEntry> entries{entry};
    EXPECT_EQ(to_hex(encode_addresses(entries)),
              "01"                                  // one entry
              "01000000"                            // time
              "0100000000000000"                    // services: NODE_NETWORK
              "00000000000000000000ffff01020304"    // 1.2.3.4 as IPv4-mapped IPv6
              "479d");                              // port 18333, big-endian
    EXPECT_EQ(decode_addresses(encode_addresses(entries)), entries);
    const std::vector<AddrEntry> too_many(kMaxAddresses + 1, entry);
    EXPECT_THROW(decode_addresses(encode_addresses(too_many)), DeserializeError);
}
