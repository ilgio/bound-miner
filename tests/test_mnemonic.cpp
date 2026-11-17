// Milestone 17: the wallet's seed as 24 words (wallet/mnemonic.h).

#include "crypto/sha256.h"
#include "util/hex.h"
#include "wallet/mnemonic.h"
#include "wallet/wallet.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

using namespace bound;

namespace {

Seed seed_of(std::string_view hex) {
    const std::optional<Bytes> bytes = from_hex(hex);
    Seed seed{};
    if (bytes && bytes->size() == seed.size()) std::copy(bytes->begin(), bytes->end(), seed.begin());
    return seed;
}

std::string problem_with(std::string_view text) {
    std::string problem;
    EXPECT_FALSE(parse_seed(text, &problem).has_value()) << text;
    return problem;
}

constexpr std::string_view kLegal =
    "legal winner thank year wave sausage worth useful legal winner thank year wave sausage worth useful legal winner thank "
    "year wave sausage worth title";

}  // namespace

// BIP39's own list, byte for byte: the SHA-256 of english.txt (one word a line) in the BIPs repository.
TEST(Mnemonic, TheWordListIsBip39s) {
    std::string file;
    for (const std::string_view w : kWordList) {
        file += w;
        file += '\n';
    }
    const Digest32 digest = sha256(ByteSpan(reinterpret_cast<const std::uint8_t*>(file.data()), file.size()));
    EXPECT_EQ(to_hex(digest), "2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda");
    EXPECT_TRUE(std::is_sorted(kWordList.begin(), kWordList.end()));
    std::set<std::string_view> beginnings;
    for (const std::string_view w : kWordList) beginnings.insert(w.substr(0, 4));
    EXPECT_EQ(beginnings.size(), kWordList.size());  // four letters always tell the words apart
    EXPECT_EQ(word_index("abandon"), 0u);
    EXPECT_EQ(word_index("zoo"), 2047u);
    EXPECT_EQ(word_index("aban"), std::nullopt);
    EXPECT_EQ(word_index("Abandon"), std::nullopt);
}

// BIP39's published vectors for 256 bits (Trezor's vectors.json), and one from a Python
// implementation of the encoding over the same list.
TEST(Mnemonic, MatchesBip39sVectors) {
    const std::pair<std::string_view, std::string> vectors[] = {
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
         "abandon abandon abandon abandon abandon abandon abandon abandon abandon art"},
        {"7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f", std::string(kLegal)},
        {"8080808080808080808080808080808080808080808080808080808080808080",
         "letter advice cage absurd amount doctor acoustic avoid letter advice cage absurd amount doctor acoustic avoid "
         "letter advice cage absurd amount doctor acoustic bless"},
        {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo vote"},
        {"52bfa9747f47bd8af9c84ba20a686641aeab1380eb771a1f366718fe2379800a",
         "famous wool fringe write knock shell transfer annual pear fashion book local turkey mean admit humble borrow whale "
         "oil cradle timber rubber ability exist"},
    };
    for (const auto& [hex, words] : vectors) {
        const Seed seed = seed_of(hex);
        EXPECT_EQ(seed_to_words(seed), words) << hex;
        EXPECT_EQ(seed_from_words(words), seed) << hex;
        EXPECT_EQ(parse_seed(words), seed) << hex;
        EXPECT_EQ(parse_seed(hex), seed) << hex;
    }
}

TEST(Mnemonic, EverySeedComesBack) {
    for (int i = 0; i < 300; ++i) {
        const Seed seed = random_seed();
        const std::string words = seed_to_words(seed);
        EXPECT_EQ(std::count(words.begin(), words.end(), ' '), 23);
        EXPECT_EQ(seed_from_words(words), seed) << words;
    }
}

TEST(Mnemonic, ForgivesCaseAndSpacing) {
    const Seed seed = seed_of("7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f");
    EXPECT_EQ(parse_seed("  LEGAL Winner thank\tyear\nwave sausage worth useful\r\nlegal winner thank year wave sausage worth useful "
                         "legal winner thank year wave sausage worth title \n"),
              seed);
    // The hex older versions showed, in its groups of eight, any case.
    EXPECT_EQ(parse_seed("7F7F7F7F 7f7f7f7f  7f7f7f7f 7f7f7f7f\n7f7f7f7f 7f7f7f7f  7f7f7f7f 7f7f7f7f"), seed);
}

TEST(Mnemonic, SaysWhatsWrong) {
    std::string legal(kLegal);
    EXPECT_EQ(problem_with(legal.substr(0, legal.rfind(' '))), "23 of 24 words.");
    EXPECT_EQ(problem_with(legal + " zoo"), "25 of 24 words.");
    EXPECT_EQ(problem_with(""), "0 of 24 words.");
    EXPECT_EQ(problem_with("legal winner thank yeer"), "Word 4, \"yeer\", isn't one of the seed words.");
    EXPECT_EQ(problem_with("legal winner éclair"), "Word 3, \"éclair\", isn't one of the seed words.");

    // The checksum: a changed word, two swapped, or another last word.
    const std::string checksum = "The words don't fit together: one of them is wrong, or two are in each other's place.";
    std::string changed = legal;
    changed.replace(changed.find("sausage"), 7, "sauce");
    EXPECT_EQ(problem_with(changed), checksum);
    EXPECT_EQ(problem_with("winner legal" + legal.substr(std::string_view("legal winner").size())), checksum);
    EXPECT_EQ(problem_with(legal.substr(0, legal.rfind(' ')) + " zoo"), checksum);

    // Hex: the length, and words mixed with digits.
    EXPECT_EQ(problem_with("7f7f7f7f"), "8 of 64 hex digits.");
    EXPECT_EQ(problem_with("legal winner 7f"), "Only the 24 words, or 64 hex digits (0-9 and a-f).");
}
