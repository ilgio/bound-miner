#pragma once

#include "core/block.h"
#include "core/uint256.h"
#include "net/socket.h"
#include "util/bytes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bound {

// Milestone 6: the peer-to-peer wire protocol. Bitcoin's framing, with BOUND's own messages:
//   magic (4) | command (12 bytes of ASCII, NUL-padded) | payload length u32 |
//   checksum (the first 4 bytes of sha256d(payload)) | payload
// Every integer is little-endian except a NetAddress's port, which is big-endian as in Bitcoin.

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kMessageHeaderSize = 24;
inline constexpr std::size_t kMaxMessageSize = 4 << 20;  // four times the largest block
inline constexpr std::size_t kMaxHeaders = 2000;          // per headers message, as in Bitcoin
inline constexpr std::size_t kMaxLocator = 101;
inline constexpr std::size_t kMaxInventory = 50000;
inline constexpr std::size_t kMaxAddresses = 1000;
inline constexpr std::size_t kMaxUserAgent = 256;
inline constexpr std::uint64_t kNodeNetwork = 1;  // service bit: serves the full chain

using Magic = std::array<std::uint8_t, 4>;

// A peer broke the framing rules; the connection must go.
struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Message {
    std::string command;
    Bytes payload;
};

namespace command {
inline constexpr std::string_view kVersion = "version";
inline constexpr std::string_view kVerack = "verack";
inline constexpr std::string_view kPing = "ping";
inline constexpr std::string_view kPong = "pong";
inline constexpr std::string_view kGetHeaders = "getheaders";
inline constexpr std::string_view kHeaders = "headers";
inline constexpr std::string_view kGetData = "getdata";
inline constexpr std::string_view kBlock = "block";
inline constexpr std::string_view kNotFound = "notfound";
inline constexpr std::string_view kGetAddr = "getaddr";
inline constexpr std::string_view kAddr = "addr";
// Milestone 7: transaction relay.
inline constexpr std::string_view kInv = "inv";
inline constexpr std::string_view kTx = "tx";
}  // namespace command

// A whole message, ready to send. Precondition: command is 1 to 12 printable ASCII characters.
Bytes frame_message(const Magic& magic, std::string_view command, ByteSpan payload);

// Cuts a byte stream into messages. Throws ProtocolError on a wrong magic, a malformed command, a
// payload over kMaxMessageSize or a wrong checksum.
class MessageReader {
public:
    explicit MessageReader(const Magic& magic) : magic_(magic) {}
    void feed(ByteSpan data);
    std::optional<Message> next();  // the next complete message, if one has arrived
    std::size_t buffered() const { return buf_.size() - pos_; }  // bytes fed and not yet handed out

private:
    Magic magic_;
    Bytes buf_;
    std::size_t pos_ = 0;  // buf_[pos_..] hasn't been consumed yet
};

// The payloads. Each decode throws DeserializeError if the payload is malformed, has trailing
// bytes, or holds more entries than the limits above.

struct VersionMsg {
    std::uint32_t version = kProtocolVersion;
    std::uint64_t services = kNodeNetwork;
    std::int64_t time = 0;
    NetAddress addr_you{};          // the receiver's address as the sender sees it (Bitcoin's addr_recv)
    std::uint16_t listen_port = 0;  // the port the sender takes connections on; 0 if it doesn't
    std::uint64_t nonce = 0;        // random per connection: getting ours back means we dialled ourselves
    std::string user_agent{};
    std::int32_t height = 0;  // the sender's active height

    friend bool operator==(const VersionMsg&, const VersionMsg&) = default;
};
Bytes encode_version(const VersionMsg& v);
VersionMsg decode_version(ByteSpan payload);

// ping and pong carry one nonce.
Bytes encode_nonce(std::uint64_t nonce);
std::uint64_t decode_nonce(ByteSpan payload);

// "Send me headers after the first of these hashes that you have on your active chain, up to
// `stop` (or as many as fit)." The locator is block_locator()'s.
struct GetHeadersMsg {
    std::vector<uint256> locator;
    uint256 stop;  // zero: no stop

    friend bool operator==(const GetHeadersMsg&, const GetHeadersMsg&) = default;
};
Bytes encode_getheaders(const GetHeadersMsg& m);
GetHeadersMsg decode_getheaders(ByteSpan payload);

// compact_size count, then 80-byte headers (Bitcoin also sends a zero tx count per header; BOUND doesn't).
Bytes encode_headers(std::span<const BlockHeader> headers);
std::vector<BlockHeader> decode_headers(ByteSpan payload);

// inv, getdata and notfound: what is on offer, what is being asked for, or what can't be sent.
// Transactions go by wtxid, as with Bitcoin's BIP339: a copy with broken signatures has the same
// txid as the real one, but not the same wtxid, so it can't stand in for it.
enum class InvType : std::uint32_t {
    Block = 2,      // Bitcoin's MSG_BLOCK; blocks are only fetched this way, never announced
    WitnessTx = 5,  // Bitcoin's MSG_WTX: a transaction, by wtxid
};
struct InvItem {
    InvType type = InvType::Block;
    uint256 hash;

    friend bool operator==(const InvItem&, const InvItem&) = default;
};
Bytes encode_inventory(std::span<const InvItem> items);
std::vector<InvItem> decode_inventory(ByteSpan payload);  // unknown types decode as-is; the reader decides

// addr: other nodes' listening addresses, each with the last time someone saw it working.
struct AddrEntry {
    std::uint32_t time = 0;
    std::uint64_t services = kNodeNetwork;
    NetAddress addr;

    friend bool operator==(const AddrEntry&, const AddrEntry&) = default;
};
Bytes encode_addresses(std::span<const AddrEntry> entries);
std::vector<AddrEntry> decode_addresses(ByteSpan payload);

}  // namespace bound
