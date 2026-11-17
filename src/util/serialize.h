#pragma once

#include "util/bytes.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace bound {

// Thrown by ByteReader, and by every deserialize function built on it, on truncated or
// malformed input. Anything that arrives from the network goes through code that can throw this.
struct DeserializeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Append-only encoder for Bitcoin's wire format: integers little-endian, signed ones two's
// complement.
class ByteWriter {
public:
    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i32(std::int32_t v);
    void i64(std::int64_t v);

    // Raw bytes, no length prefix.
    void bytes(ByteSpan b);

    // Bitcoin's CompactSize: n < 0xfd -> 1 byte; n <= 0xffff -> 0xfd + u16;
    // n <= 0xffffffff -> 0xfe + u32; otherwise 0xff + u64.
    void compact_size(std::uint64_t n);

    // compact_size(b.size()) followed by the bytes.
    void var_bytes(ByteSpan b);

    const Bytes& data() const { return buf_; }
    Bytes take() && { return std::move(buf_); }

private:
    Bytes buf_;
};

// Decoder over a borrowed buffer, which must outlive the reader. Every read throws
// DeserializeError if the input runs out; after a throw, discard the reader.
class ByteReader {
public:
    explicit ByteReader(ByteSpan data) : data_(data) {}

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int32_t i32();
    std::int64_t i64();

    // The next n bytes, as a view into the underlying buffer (no copy).
    ByteSpan bytes(std::size_t n);

    // Throws on non-canonical encodings (a value that fits a shorter form), as Bitcoin does;
    // otherwise two different byte strings could decode to the same object.
    std::uint64_t compact_size();

    // A compact_size length, then that many bytes. Throws if the length exceeds max_len, before
    // reading or allocating anything: a peer must not be able to make us allocate 2^64 bytes.
    Bytes var_bytes(std::size_t max_len);

    std::size_t remaining() const { return data_.size() - pos_; }
    bool empty() const { return remaining() == 0; }

    // Throws unless every byte has been consumed. Call it at the end of a top-level decode.
    void expect_end() const;

private:
    ByteSpan data_;
    std::size_t pos_ = 0;
};

}  // namespace bound
