#include "net/protocol.h"

#include "crypto/sha256.h"
#include "util/serialize.h"

#include <algorithm>
#include <utility>

namespace bound {

namespace {

constexpr std::size_t kCommandSize = 12;
constexpr std::size_t kAddressSize = 18;  // 16-byte IP + port

std::array<std::uint8_t, 4> checksum(ByteSpan payload) {
    const Digest32 d = sha256d(payload);
    return {d[0], d[1], d[2], d[3]};
}

bool printable(std::uint8_t c) { return c >= 0x20 && c < 0x7f; }

void write_address(ByteWriter& w, const NetAddress& a) {
    w.bytes(a.ip);
    w.u8(static_cast<std::uint8_t>(a.port >> 8));  // big-endian, as in Bitcoin
    w.u8(static_cast<std::uint8_t>(a.port));
}

NetAddress read_address(ByteReader& r) {
    NetAddress a;
    const ByteSpan ip = r.bytes(a.ip.size());
    std::copy(ip.begin(), ip.end(), a.ip.begin());
    const std::uint8_t hi = r.u8();
    a.port = static_cast<std::uint16_t>(hi << 8 | r.u8());
    return a;
}

uint256 read_hash(ByteReader& r) {
    const ByteSpan b = r.bytes(32);
    Digest32 d;
    std::copy(b.begin(), b.end(), d.begin());
    return uint256(d);
}

// A compact_size count, checked against both the protocol limit and what's left to read.
std::size_t read_count(ByteReader& r, std::size_t limit, std::size_t item_size) {
    const std::uint64_t n = r.compact_size();
    if (n > limit) throw DeserializeError("too many entries");
    if (n > r.remaining() / item_size) throw DeserializeError("entry count exceeds the payload");
    return static_cast<std::size_t>(n);
}

}  // namespace

Bytes frame_message(const Magic& magic, std::string_view command, ByteSpan payload) {
    ByteWriter w;
    w.bytes(magic);
    std::array<std::uint8_t, kCommandSize> name{};
    std::copy(command.begin(), command.end(), name.begin());
    w.bytes(name);
    w.u32(static_cast<std::uint32_t>(payload.size()));
    w.bytes(checksum(payload));
    w.bytes(payload);
    return std::move(w).take();
}

void MessageReader::feed(ByteSpan data) {
    if (pos_ > 0 && pos_ == buf_.size()) {
        buf_.clear();
        pos_ = 0;
    } else if (pos_ > (1 << 20)) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), data.begin(), data.end());
}

std::optional<Message> MessageReader::next() {
    if (buf_.size() - pos_ < kMessageHeaderSize) return std::nullopt;
    ByteReader header(ByteSpan(buf_).subspan(pos_, kMessageHeaderSize));
    if (!std::ranges::equal(header.bytes(magic_.size()), magic_)) throw ProtocolError("wrong network magic");

    const ByteSpan name = header.bytes(kCommandSize);
    const auto end = std::find(name.begin(), name.end(), std::uint8_t{0});
    if (end == name.begin() || !std::all_of(name.begin(), end, printable) ||
        !std::all_of(end, name.end(), [](std::uint8_t c) { return c == 0; })) {
        throw ProtocolError("malformed command");
    }
    const std::uint32_t length = header.u32();
    if (length > kMaxMessageSize) throw ProtocolError("message too large");
    const ByteSpan sum = header.bytes(4);
    if (buf_.size() - pos_ < kMessageHeaderSize + length) return std::nullopt;

    const ByteSpan payload = ByteSpan(buf_).subspan(pos_ + kMessageHeaderSize, length);
    if (!std::ranges::equal(checksum(payload), sum)) throw ProtocolError("bad checksum");
    Message m{std::string(name.begin(), end), Bytes(payload.begin(), payload.end())};
    pos_ += kMessageHeaderSize + length;
    return m;
}

Bytes encode_version(const VersionMsg& v) {
    ByteWriter w;
    w.u32(v.version);
    w.u64(v.services);
    w.i64(v.time);
    write_address(w, v.addr_you);
    w.u16(v.listen_port);
    w.u64(v.nonce);
    w.var_bytes(bytes_of(v.user_agent));
    w.i32(v.height);
    return std::move(w).take();
}

VersionMsg decode_version(ByteSpan payload) {
    ByteReader r(payload);
    VersionMsg v;
    v.version = r.u32();
    v.services = r.u64();
    v.time = r.i64();
    v.addr_you = read_address(r);
    v.listen_port = r.u16();
    v.nonce = r.u64();
    const Bytes agent = r.var_bytes(kMaxUserAgent);
    v.user_agent.assign(agent.begin(), agent.end());
    v.height = r.i32();
    r.expect_end();
    return v;
}

Bytes encode_nonce(std::uint64_t nonce) {
    ByteWriter w;
    w.u64(nonce);
    return std::move(w).take();
}

std::uint64_t decode_nonce(ByteSpan payload) {
    ByteReader r(payload);
    const std::uint64_t nonce = r.u64();
    r.expect_end();
    return nonce;
}

Bytes encode_getheaders(const GetHeadersMsg& m) {
    ByteWriter w;
    w.compact_size(m.locator.size());
    for (const uint256& h : m.locator) w.bytes(h.bytes());
    w.bytes(m.stop.bytes());
    return std::move(w).take();
}

GetHeadersMsg decode_getheaders(ByteSpan payload) {
    ByteReader r(payload);
    GetHeadersMsg m;
    const std::size_t n = read_count(r, kMaxLocator, 32);
    m.locator.reserve(n);
    for (std::size_t i = 0; i < n; ++i) m.locator.push_back(read_hash(r));
    m.stop = read_hash(r);
    r.expect_end();
    return m;
}

Bytes encode_headers(std::span<const BlockHeader> headers) {
    ByteWriter w;
    w.compact_size(headers.size());
    for (const BlockHeader& h : headers) w.bytes(h.serialize());
    return std::move(w).take();
}

std::vector<BlockHeader> decode_headers(ByteSpan payload) {
    ByteReader r(payload);
    const std::size_t n = read_count(r, kMaxHeaders, BlockHeader::kSize);
    std::vector<BlockHeader> headers;
    headers.reserve(n);
    for (std::size_t i = 0; i < n; ++i) headers.push_back(BlockHeader::deserialize(r.bytes(BlockHeader::kSize)));
    r.expect_end();
    return headers;
}

Bytes encode_inventory(std::span<const InvItem> items) {
    ByteWriter w;
    w.compact_size(items.size());
    for (const InvItem& item : items) {
        w.u32(static_cast<std::uint32_t>(item.type));
        w.bytes(item.hash.bytes());
    }
    return std::move(w).take();
}

std::vector<InvItem> decode_inventory(ByteSpan payload) {
    ByteReader r(payload);
    const std::size_t n = read_count(r, kMaxInventory, 36);
    std::vector<InvItem> items;
    items.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        InvItem item;
        item.type = static_cast<InvType>(r.u32());
        item.hash = read_hash(r);
        items.push_back(item);
    }
    r.expect_end();
    return items;
}

Bytes encode_addresses(std::span<const AddrEntry> entries) {
    ByteWriter w;
    w.compact_size(entries.size());
    for (const AddrEntry& e : entries) {
        w.u32(e.time);
        w.u64(e.services);
        write_address(w, e.addr);
    }
    return std::move(w).take();
}

std::vector<AddrEntry> decode_addresses(ByteSpan payload) {
    ByteReader r(payload);
    const std::size_t n = read_count(r, kMaxAddresses, 12 + kAddressSize);
    std::vector<AddrEntry> entries;
    entries.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        AddrEntry e;
        e.time = r.u32();
        e.services = r.u64();
        e.addr = read_address(r);
        entries.push_back(e);
    }
    r.expect_end();
    return entries;
}

}  // namespace bound
