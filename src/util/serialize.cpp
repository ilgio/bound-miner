#include "util/serialize.h"

namespace bound {

namespace {

template <typename T>
void put_le(Bytes& buf, T v) {
    for (std::size_t i = 0; i < sizeof(T); ++i) buf.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

template <typename T>
T get_le(ByteSpan b) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) v |= std::uint64_t{b[i]} << (8 * i);
    return static_cast<T>(v);
}

}  // namespace

void ByteWriter::u8(std::uint8_t v) { buf_.push_back(v); }
void ByteWriter::u16(std::uint16_t v) { put_le(buf_, v); }
void ByteWriter::u32(std::uint32_t v) { put_le(buf_, v); }
void ByteWriter::u64(std::uint64_t v) { put_le(buf_, v); }
void ByteWriter::i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
void ByteWriter::i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
void ByteWriter::bytes(ByteSpan b) { buf_.insert(buf_.end(), b.begin(), b.end()); }

void ByteWriter::compact_size(std::uint64_t n) {
    if (n < 0xfd) {
        u8(static_cast<std::uint8_t>(n));
    } else if (n <= 0xffff) {
        u8(0xfd);
        u16(static_cast<std::uint16_t>(n));
    } else if (n <= 0xffffffff) {
        u8(0xfe);
        u32(static_cast<std::uint32_t>(n));
    } else {
        u8(0xff);
        u64(n);
    }
}

void ByteWriter::var_bytes(ByteSpan b) {
    compact_size(b.size());
    bytes(b);
}

ByteSpan ByteReader::bytes(std::size_t n) {
    if (n > remaining()) throw DeserializeError("unexpected end of data");
    const ByteSpan out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
}

std::uint8_t ByteReader::u8() { return bytes(1)[0]; }
std::uint16_t ByteReader::u16() { return get_le<std::uint16_t>(bytes(2)); }
std::uint32_t ByteReader::u32() { return get_le<std::uint32_t>(bytes(4)); }
std::uint64_t ByteReader::u64() { return get_le<std::uint64_t>(bytes(8)); }
std::int32_t ByteReader::i32() { return static_cast<std::int32_t>(u32()); }
std::int64_t ByteReader::i64() { return static_cast<std::int64_t>(u64()); }

std::uint64_t ByteReader::compact_size() {
    const std::uint8_t tag = u8();
    std::uint64_t n = 0;
    std::uint64_t min = 0;
    switch (tag) {
    case 0xfd: n = u16(); min = 0xfd; break;
    case 0xfe: n = u32(); min = 0x10000; break;
    case 0xff: n = u64(); min = 0x100000000; break;
    default: return tag;
    }
    if (n < min) throw DeserializeError("non-canonical compact size");
    return n;
}

Bytes ByteReader::var_bytes(std::size_t max_len) {
    const std::uint64_t n = compact_size();
    if (n > max_len) throw DeserializeError("length exceeds limit");
    const ByteSpan b = bytes(static_cast<std::size_t>(n));
    return Bytes(b.begin(), b.end());
}

void ByteReader::expect_end() const {
    if (!empty()) throw DeserializeError("trailing data");
}

}  // namespace bound
