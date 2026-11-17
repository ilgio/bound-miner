#pragma once

// Milestone 17: a wallet's seed written down as 24 words, the way Bitcoin wallets show theirs.
//
// The encoding and the word list are BIP39's: the seed's 256 bits and the first 8 bits of its
// SHA-256 make 264 bits, cut into 24 groups of 11, each naming one of 2,048 words. The last word
// carries the checksum, so a wrong word, or two swapped, is caught 255 times in 256. The words are
// the seed itself, nothing more: a wallet from before them has the same seed, now shown as words.
//
// BIP39 wallets (Trust Wallet, MetaMask, Electrum's BIP39 mode...) stretch the words into a
// different seed and derive their keys another way, so the same 24 words open an unrelated, empty
// wallet there. Only BOUND's wallet restores BOUND coins from them.

#include "crypto/ed25519.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace bound {

inline constexpr std::size_t kSeedWords = 24;

// BIP39's English list, in its order (wallet/wordlist.cpp). Sorted, and no two words share their
// first four letters.
extern const std::array<std::string_view, 2048> kWordList;

// The 24 words, lowercase, one space between.
std::string seed_to_words(const Seed& seed);

// The seed back from its words: any case, any whitespace around and between them. nullopt for a
// wrong number of words, a word not on the list, or words that fail the checksum; `problem`, if
// given, then says which, in a sentence for people.
std::optional<Seed> seed_from_words(std::string_view text, std::string* problem = nullptr);

// What restoring a wallet takes: the 24 words, or the 64 hex digits that versions before 1.0
// showed (whitespace anywhere is ignored). Text with a digit in it is read as hex: words have none.
std::optional<Seed> parse_seed(std::string_view text, std::string* problem = nullptr);

// Where `word` is on the list, or nullopt. Lowercase only.
std::optional<std::size_t> word_index(std::string_view word);

}  // namespace bound
