# Milestone 17: the wallet's 24 words

**What it adds:** the wallet's backup is 24 words, as in Bitcoin wallets, instead of 64 hex digits.

## Decided (2026-11-16, by the project owner)

Asked to make the wallet "much better", the project owner chose the 24-word backup first (from a
24-word seed, a QR code on Receive, a mainnet/testnet switch in the app, and contacts with
notifications).

## How

- **The seed doesn't change, only how it's written down.** The 32-byte seed and the first byte of
  its SHA-256 make 264 bits, cut into 24 groups of 11; each group names a word from
  [BIP39](https://github.com/bitcoin/bips/blob/master/bip-0039.mediawiki)'s English list of 2,048.
  That's BIP39's encoding exactly, so its published test vectors check it. A wallet made before
  shows its old seed as words; restoring takes the words or the old 64 hex digits.
- **The last word is a checksum.** A misspelt word is named ("Word 4, "yeer", isn't one of the
  seed words."); a wrong word or two swapped are caught 255 times in 256.
- **Not interchangeable with other wallets.** BIP39 wallets (Trust Wallet, MetaMask...) stretch the
  words into another seed and derive keys their own way, so the same words open an empty wallet
  there. Only BOUND's wallet restores BOUND coins from them, and the app says so.

## In the code

- `wallet/mnemonic.{h,cpp}`: `seed_to_words`, `seed_from_words` and `parse_seed` (words or hex), with
  the reason when the text isn't a seed. Buffers holding the seed or its words are wiped after use.
- `wallet/wordlist.cpp`: the list, generated from `bip-0039/english.txt`; a test checks its SHA-256.
- RPC: `createwallet` and `dumpseed` return `words` (and `seed`, the hex); `restorewallet` takes
  either.
- The wallet app shows the words numbered, in three columns, and its restore dialog counts them as
  they're typed and points at a misspelt one.

## Tests

BIP39's vectors for 256 bits and one more from a Python implementation; 300 random seeds there and
back; case and whitespace; every kind of mistake and its message; the RPC round trip creates a
wallet, reads its words back with `dumpseed` and restores another wallet from them.
