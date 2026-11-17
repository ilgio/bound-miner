#include "util/hex.h"
#include "util/serialize.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

using namespace bound;

namespace {

Bytes unhex(std::string_view hex) { return from_hex(hex).value(); }

}  // namespace

TEST(Serialize, IntegersAreLittleEndian) {
    ByteWriter w;
    w.u8(0x01);
    w.u16(0x0203);
    w.u32(0x04050607);
    w.u64(0x08090a0b0c0d0e0f);
    EXPECT_EQ(to_hex(w.data()), "01" "0302" "07060504" "0f0e0d0c0b0a0908");
}

TEST(Serialize, SignedIntegersAreTwosComplement) {
    ByteWriter w;
    w.i32(-2);
    w.i64(-1);
    EXPECT_EQ(to_hex(w.data()), "feffffff" "ffffffffffffffff");
}

TEST(Serialize, IntegersRoundTrip) {
    ByteWriter w;
    w.u8(0xab);
    w.u16(0xfffe);
    w.u32(0xdeadbeef);
    w.u64(std::numeric_limits<std::uint64_t>::max());
    w.i32(std::numeric_limits<std::int32_t>::min());
    w.i64(-1234567890123);
    const Bytes buf = std::move(w).take();

    ByteReader r(buf);
    EXPECT_EQ(r.u8(), 0xab);
    EXPECT_EQ(r.u16(), 0xfffe);
    EXPECT_EQ(r.u32(), 0xdeadbeefu);
    EXPECT_EQ(r.u64(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(r.i32(), std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(r.i64(), -1234567890123);
    EXPECT_TRUE(r.empty());
    EXPECT_NO_THROW(r.expect_end());
}

TEST(Serialize, CompactSizeBoundaries) {
    struct Case {
        std::uint64_t n;
        const char* hex;
    };
    const Case cases[] = {
        {0, "00"},
        {0xfc, "fc"},
        {0xfd, "fdfd00"},
        {0xffff, "fdffff"},
        {0x10000, "fe00000100"},
        {0xffffffff, "feffffffff"},
        {0x100000000, "ff0000000001000000"},
        {std::numeric_limits<std::uint64_t>::max(), "ffffffffffffffffff"},
    };
    for (const Case& c : cases) {
        ByteWriter w;
        w.compact_size(c.n);
        EXPECT_EQ(to_hex(w.data()), c.hex) << "n=" << c.n;

        const Bytes buf = unhex(c.hex);
        ByteReader r(buf);
        EXPECT_EQ(r.compact_size(), c.n) << "hex=" << c.hex;
        EXPECT_TRUE(r.empty()) << "hex=" << c.hex;
    }
}

TEST(Serialize, CompactSizeRejectsNonCanonical) {
    // Each encodes a value that fits the next-shorter form.
    for (const char* hex : {"fdfc00", "feffff0000", "ffffffffff00000000"}) {
        const Bytes buf = unhex(hex);
        ByteReader r(buf);
        EXPECT_THROW(r.compact_size(), DeserializeError) << "hex=" << hex;
    }
}

TEST(Serialize, TruncatedInputThrows) {
    const Bytes two = unhex("0102");
    {
        ByteReader r(two);
        EXPECT_THROW(r.u32(), DeserializeError);
    }
    {
        ByteReader r(two);
        EXPECT_THROW(r.bytes(3), DeserializeError);
    }
    {
        const Bytes buf = unhex("fd01");
        ByteReader r(buf);
        EXPECT_THROW(r.compact_size(), DeserializeError);
    }
    {
        ByteReader r(ByteSpan{});
        EXPECT_THROW(r.u8(), DeserializeError);
    }
}

TEST(Serialize, BytesIsAViewAndAdvances) {
    const Bytes buf = unhex("aabbccdd");
    ByteReader r(buf);
    const ByteSpan first = r.bytes(3);
    ASSERT_EQ(first.size(), 3u);
    EXPECT_EQ(first.data(), buf.data());  // a view, not a copy
    EXPECT_EQ(r.remaining(), 1u);
    EXPECT_EQ(r.u8(), 0xdd);
}

TEST(Serialize, VarBytesRoundTrip) {
    ByteWriter w;
    w.var_bytes(bytes_of("hello"));
    EXPECT_EQ(to_hex(w.data()), "05" "68656c6c6f");

    ByteReader r(w.data());
    EXPECT_EQ(r.var_bytes(5), (Bytes{'h', 'e', 'l', 'l', 'o'}));
    EXPECT_TRUE(r.empty());
}

TEST(Serialize, VarBytesEnforcesMaxLength) {
    ByteWriter w;
    w.var_bytes(bytes_of("hello"));
    ByteReader r(w.data());
    EXPECT_THROW(r.var_bytes(4), DeserializeError);
}

TEST(Serialize, VarBytesLengthBeyondInputThrows) {
    // Claims 2^32 - 1 bytes follow, then provides one.
    const Bytes buf = unhex("feffffffff" "00");
    ByteReader r(buf);
    EXPECT_THROW(r.var_bytes(std::numeric_limits<std::size_t>::max()), DeserializeError);
}

TEST(Serialize, ExpectEndThrowsOnTrailingBytes) {
    const Bytes buf = unhex("0001");
    ByteReader r(buf);
    r.u8();
    EXPECT_THROW(r.expect_end(), DeserializeError);
    r.u8();
    EXPECT_NO_THROW(r.expect_end());
}
