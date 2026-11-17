#include "core/arith_uint256.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string_view>

using namespace bound;

namespace {

// Values computed independently with Python's arbitrary-precision integers. a and b are
// sha256("BOUND arith a" / "BOUND arith b"); d is the first 12 bytes of sha256("BOUND arith d").
arith_uint256 A(std::string_view display_hex) { return arith_uint256(U(display_hex)); }
arith_uint256 a() { return A("c6a176a2e7119d822532f1d03743dd2c8931d01270dcdf90a77e816a1c8bf9a3"); }
arith_uint256 b() { return A("2f48b585a004dfca352ae5a17e94543ad02886a9fd2a17c45581b8701434db4c"); }
arith_uint256 d() { return A("0000000000000000000000000000000000000000b1d8cf8ee95b96f5f2bc2c9d"); }
arith_uint256 all_ones() { return A("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); }

}  // namespace

TEST(Arith256, RoundTripsThroughUint256) {
    const uint256 v = U("c6a176a2e7119d822532f1d03743dd2c8931d01270dcdf90a77e816a1c8bf9a3");
    EXPECT_EQ(arith_uint256(v).to_uint256(), v);
    EXPECT_EQ(arith_uint256(0x0123456789abcdefULL).to_uint256(),
              U("0000000000000000000000000000000000000000000000000123456789abcdef"));
}

TEST(Arith256, ComparesNumerically) {
    EXPECT_LT(arith_uint256(1), arith_uint256(2));
    EXPECT_LT(arith_uint256(0xffffffffULL), arith_uint256(0x100000000ULL));  // across a limb
    EXPECT_GT(a(), b());
    EXPECT_LT(d(), b());
    EXPECT_EQ(a(), a());
}

TEST(Arith256, AddsAndSubtractsWithCarry) {
    EXPECT_EQ(a() + b(), A("f5ea2c2887167d4c5a5dd771b5d83167595a56bc6e06f754fd0039da30c0d4ef"));
    EXPECT_EQ(a() - b(), A("9758c11d470cbdb7f0080c2eb8af88f1b909496873b2c7cc51fcc8fa08571e57"));
    EXPECT_EQ(b() - a(), A("68a73ee2b8f342480ff7f3d14750770e46f6b6978c4d3833ae033705f7a8e1a9"));  // wraps
    EXPECT_EQ(arith_uint256(0xffffffffULL) + 1, arith_uint256(0x100000000ULL));
    EXPECT_EQ(all_ones() + 1, arith_uint256(0));
    EXPECT_EQ(arith_uint256(0) - 1, all_ones());
    EXPECT_EQ(~arith_uint256(0), all_ones());
}

TEST(Arith256, Shifts) {
    EXPECT_EQ(a() << 100, A("743dd2c8931d01270dcdf90a77e816a1c8bf9a30000000000000000000000000"));
    EXPECT_EQ(a() >> 100, A("0000000000000000000000000c6a176a2e7119d822532f1d03743dd2c8931d01"));
    EXPECT_EQ(a() << 37, A("e233b044a65e3a06e87ba591263a024e1b9bf214efd02d43917f346000000000"));
    EXPECT_EQ(a() >> 37, A("0000000006350bb517388cec1129978e81ba1ee964498e809386e6fc853bf40b"));
    EXPECT_EQ((arith_uint256(1) << 255) >> 255, arith_uint256(1));
    EXPECT_EQ(a() << 256, arith_uint256(0));
    EXPECT_EQ(a() >> 256, arith_uint256(0));
    EXPECT_EQ(a() << 0, a());
}

TEST(Arith256, Divides) {
    EXPECT_EQ(a() / d(), A("0000000000000000000000011deac2e1ac71a5b779cf4c91b97d4156a8f41494"));
    EXPECT_EQ(a() / b(), arith_uint256(4));
    EXPECT_EQ(b() / a(), arith_uint256(0));
    EXPECT_EQ(all_ones() / 3, A("5555555555555555555555555555555555555555555555555555555555555555"));
    EXPECT_EQ(a() / a(), arith_uint256(1));
    EXPECT_THROW(a() / arith_uint256(0), std::domain_error);
}

TEST(Arith256, CountsBits) {
    EXPECT_EQ(arith_uint256(0).bits(), 0);
    EXPECT_EQ(arith_uint256(1).bits(), 1);
    EXPECT_EQ(arith_uint256(0x100000000ULL).bits(), 33);
    EXPECT_EQ(a().bits(), 256);
    EXPECT_EQ(d().bits(), 96);
}

TEST(Arith256, MulDivIsExactBeyond256Bits) {
    // (2^255 + 12345) * 7 needs 258 bits; the quotient by 8 fits.
    EXPECT_EQ(mul_div((arith_uint256(1) << 255) + 12345, 7, 8),
              A("7000000000000000000000000000000000000000000000000000000000002a31"));
    // (a >> 3) * 345600 needs 272 bits.
    EXPECT_EQ(mul_div(a() >> 3, 345600, 86401),
              A("63506ffc8b701f3446673393615bdc1c40b89288f9d3b797ff19e5fcd0159597"));
    EXPECT_EQ(mul_div(a(), 345600, 86399), std::nullopt);  // about 4a: doesn't fit
    EXPECT_EQ(mul_div(a(), 1, 1), a());
    EXPECT_THROW(mul_div(a(), 1, 0), std::domain_error);
}

TEST(Arith256, ConvertsToDouble) {
    EXPECT_EQ(arith_uint256(65537).to_double(), 65537.0);
    EXPECT_EQ((arith_uint256(1) << 200).to_double(), std::ldexp(1.0, 200));
}
