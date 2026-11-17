#pragma once

// Milestone 12: fuzz targets, as Bitcoin Core has them (src/test/fuzz). Each target takes
// arbitrary bytes and feeds them to one piece of code that reads untrusted input: a decoder, a
// parser, a peer's messages. A target must never crash, never hit undefined behaviour (sanitizers
// catch that), and never let an exception out other than the ones that code is documented to
// throw on bad input (it catches those itself). Some also check properties, e.g. that whatever
// decodes re-encodes to the same thing.
//
// bound_fuzz runs them, two ways (fuzz/main.cpp):
//   - built with clang's libFuzzer (-DBOUND_FUZZ=libfuzzer): coverage-guided, the target named
//     by the FUZZ environment variable: FUZZ=peer bound_fuzz -max_total_time=60
//   - built plainly (any compiler): bound_fuzz [target|all] [--runs n] [files...] mutates each
//     target's seed inputs at random, and runs any files given (crash reproducers).

#include "util/serialize.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bound::fuzz {

using Data = std::span<const std::uint8_t>;

struct Target {
    void (*run)(Data data) = nullptr;
    std::vector<Bytes> (*seeds)() = nullptr;  // valid examples to start mutating from; may be null
};

std::map<std::string, Target>& registry();

struct Register {
    Register(const char* name, Target target) { registry()[name] = target; }
};

// FUZZ_TARGET(name) { ...uses `data`... } defines and registers a target; FUZZ_TARGET_SEEDED
// adds a function returning seed inputs.
#define BOUND_FUZZ_CAT2(a, b) a##b
#define BOUND_FUZZ_CAT(a, b) BOUND_FUZZ_CAT2(a, b)
#define FUZZ_TARGET(name)                                                                          \
    static void fuzz_##name(::bound::fuzz::Data data);                                             \
    static const ::bound::fuzz::Register BOUND_FUZZ_CAT(register_, name)(#name, {fuzz_##name, nullptr}); \
    static void fuzz_##name(::bound::fuzz::Data data)
#define FUZZ_TARGET_SEEDED(name, seed_function)                                                    \
    static void fuzz_##name(::bound::fuzz::Data data);                                             \
    static const ::bound::fuzz::Register BOUND_FUZZ_CAT(register_, name)(#name, {fuzz_##name, seed_function}); \
    static void fuzz_##name(::bound::fuzz::Data data)

// Takes typed values off the front of the input; once it runs out, everything reads as zero.
class Input {
public:
    explicit Input(Data data) : data_(data) {}

    bool empty() const { return pos_ >= data_.size(); }
    std::size_t remaining() const { return data_.size() - pos_; }

    std::uint8_t u8() { return pos_ < data_.size() ? data_[pos_++] : 0; }
    std::uint16_t u16() { return static_cast<std::uint16_t>(u8() | (u8() << 8)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(u16()) | (static_cast<std::uint32_t>(u16()) << 16); }
    std::uint64_t u64() { return static_cast<std::uint64_t>(u32()) | (static_cast<std::uint64_t>(u32()) << 32); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    bool flag() { return (u8() & 1) != 0; }
    // An index below n (n > 0).
    std::size_t pick(std::size_t n) { return u16() % n; }

    // Up to `max` bytes, the length taken from the input.
    Bytes bytes(std::size_t max) {
        // The length first, in its own statement: function arguments may be evaluated in any
        // order (MSVC goes right to left), and remaining() must come after u16() has taken its 2.
        const std::size_t wanted = u16() % (max + 1);
        const std::size_t n = std::min(wanted, remaining());
        Bytes out(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return out;
    }
    std::string text(std::size_t max) {
        const Bytes b = bytes(max);
        return std::string(b.begin(), b.end());
    }
    Bytes rest() {
        Bytes out(data_.begin() + static_cast<std::ptrdiff_t>(std::min(pos_, data_.size())), data_.end());
        pos_ = data_.size();
        return out;
    }

private:
    Data data_;
    std::size_t pos_ = 0;
};

// A target's own check failed: a property that must hold doesn't. Ends the run as a crash would.
[[noreturn]] void fail(const char* what);
#define FUZZ_ASSERT(condition)                                       \
    do {                                                            \
        if (!(condition)) ::bound::fuzz::fail("FUZZ_ASSERT(" #condition ")"); \
    } while (false)

}  // namespace bound::fuzz
