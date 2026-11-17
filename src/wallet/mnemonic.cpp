#include "wallet/mnemonic.h"

#include "crypto/sha256.h"
#include "util/hex.h"

#include <sodium.h>

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

namespace bound {

namespace {

constexpr std::size_t kBits = kSeedWords * 11;  // 264: the seed's 256 and 8 of checksum
static_assert(kBits == 256 + 8);

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool is_hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// The seed and its checksum byte, the 264 bits the words stand for.
std::array<std::uint8_t, 33> with_checksum(const Seed& seed) {
    std::array<std::uint8_t, 33> bits{};
    std::copy(seed.begin(), seed.end(), bits.begin());
    Digest32 digest = sha256(seed);
    bits[32] = digest[0];
    sodium_memzero(digest.data(), digest.size());
    return bits;
}

std::optional<Seed> fail(std::string* problem, std::string why) {
    if (problem != nullptr) *problem = std::move(why);
    return std::nullopt;
}

void wipe(std::string& s) {
    if (!s.empty()) sodium_memzero(s.data(), s.size());
}

}  // namespace

std::optional<std::size_t> word_index(std::string_view word) {
    const auto it = std::lower_bound(kWordList.begin(), kWordList.end(), word);
    if (it == kWordList.end() || *it != word) return std::nullopt;
    return static_cast<std::size_t>(it - kWordList.begin());
}

std::string seed_to_words(const Seed& seed) {
    std::array<std::uint8_t, 33> bits = with_checksum(seed);
    std::string words;
    words.reserve(kSeedWords * 9);
    for (std::size_t w = 0; w < kSeedWords; ++w) {
        std::size_t index = 0;
        for (std::size_t b = w * 11; b < w * 11 + 11; ++b) index = index << 1 | ((bits[b / 8] >> (7 - b % 8)) & 1u);
        if (w > 0) words += ' ';
        words += kWordList[index];
    }
    sodium_memzero(bits.data(), bits.size());
    return words;
}

std::optional<Seed> seed_from_words(std::string_view text, std::string* problem) {
    // The words, lowercased, wherever the whitespace falls.
    std::vector<std::string> words;
    std::string current;
    for (const char c : text) {
        if (is_space(c)) {
            if (!current.empty()) words.push_back(std::exchange(current, {}));
        } else {
            current += lower(c);
        }
    }
    if (!current.empty()) words.push_back(std::move(current));
    struct Wipe {
        std::vector<std::string>& words;
        ~Wipe() {
            for (std::string& w : words) wipe(w);
        }
    } wipe_words{words};

    std::array<std::uint8_t, 33> bits{};
    for (std::size_t w = 0; w < words.size(); ++w) {
        const std::optional<std::size_t> index = word_index(words[w]);
        if (!index) {
            const std::string shown = words[w].size() <= 12 ? words[w] : words[w].substr(0, 12) + "...";
            return fail(problem, std::format("Word {}, \"{}\", isn't one of the seed words.", w + 1, shown));
        }
        if (w >= kSeedWords) continue;
        for (std::size_t b = 0; b < 11; ++b) {
            if ((*index >> (10 - b)) & 1u) bits[(w * 11 + b) / 8] |= static_cast<std::uint8_t>(0x80u >> ((w * 11 + b) % 8));
        }
    }
    if (words.size() != kSeedWords) {
        sodium_memzero(bits.data(), bits.size());
        return fail(problem, std::format("{} of {} words.", words.size(), kSeedWords));
    }
    Seed seed;
    std::copy_n(bits.begin(), seed.size(), seed.begin());
    std::array<std::uint8_t, 33> expected = with_checksum(seed);
    const bool ok = expected[32] == bits[32];
    sodium_memzero(expected.data(), expected.size());
    sodium_memzero(bits.data(), bits.size());
    if (!ok) {
        sodium_memzero(seed.data(), seed.size());
        return fail(problem, "The words don't fit together: one of them is wrong, or two are in each other's place.");
    }
    return seed;
}

std::optional<Seed> parse_seed(std::string_view text, std::string* problem) {
    std::string compact;
    for (const char c : text) {
        if (!is_space(c)) compact += c;
    }
    const bool all_hex = !compact.empty() && std::all_of(compact.begin(), compact.end(), is_hex_digit);
    const bool hex = all_hex && (compact.size() == 64 || std::any_of(compact.begin(), compact.end(), is_digit));
    if (!hex && std::any_of(compact.begin(), compact.end(), is_digit)) {
        wipe(compact);
        return fail(problem, "Only the 24 words, or 64 hex digits (0-9 and a-f).");
    }
    if (!hex) {
        wipe(compact);
        return seed_from_words(text, problem);
    }
    if (compact.size() != 64) {
        const std::size_t size = compact.size();
        wipe(compact);
        return fail(problem, std::format("{} of 64 hex digits.", size));
    }
    std::optional<Bytes> bytes = from_hex(compact);
    wipe(compact);
    if (!bytes || bytes->size() != 32) return fail(problem, "Only the 24 words, or 64 hex digits (0-9 and a-f).");
    Seed seed;
    std::copy(bytes->begin(), bytes->end(), seed.begin());
    sodium_memzero(bytes->data(), bytes->size());
    return seed;
}

}  // namespace bound
