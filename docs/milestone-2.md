# Milestone 2: keys, transactions, blocks with bodies ✅

**What it adds:** Ed25519 keys, Base58Check addresses, signed transactions, the coinbase and block
reward, a merkle tree, full blocks, and the checks that need no chain context.

## Files

| File | What it does | Tests |
|---|---|---|
| `util/base58.*` | Base58 and Base58Check | `Base58*` |
| `crypto/ed25519.*` | move-only `KeyPair` (secret wiped on destruction) and `verify`, via libsodium | `Ed25519.*` |
| `core/address.*` | Base58Check(version ‖ 32-byte key) | `Address.*` |
| `core/amount.h` | `Amount` (int64 base units), `kCoin`, `kMaxMoney`, `money_range` | |
| `core/transaction.*` | `OutPoint`, `TxOut`, `Transaction`, wire format, txid/wtxid/sighash, signing | `Transaction.*` |
| `core/merkle.*` | tagged merkle tree, odd node promoted | `Merkle.*` |
| `core/block.*` | `Block` (header + transactions) | `Block.*` |
| `core/consensus.*` | `block_subsidy`, `check_transaction`, `check_block` | `Subsidy.*`, `CheckTransaction.*`, `CheckBlock.*` |
| `core/miner.*` | `make_coinbase` | `Coinbase.*` |

The test vectors were computed independently: RFC 8032 Ed25519 vectors, Bitcoin Core's Base58
vectors (and Satoshi's address), Python hashlib, and a pure-Python Ed25519.

## Economics (chosen by the project owner)

| | |
|---|---|
| Target block time | 60 s (`target_spacing`; retargeting arrives in milestone 4) |
| Initial reward | 5 BOUND |
| Halving | every 2,100,000 blocks (about 4 years); zero from the 64th halving, as in Bitcoin |
| Total supply | 2,099,999,972,700,000 base units = **20,999,999.727 BOUND** |
| Unit | 1 BOUND = 10^8 base units |
| Max block size | 1,000,000 serialized bytes |
| Addresses | Base58Check, version byte `0xf0` on testnet: 51 characters, always starting with `9` |

## Where BOUND differs from Bitcoin

- **No Script.** An output is `(amount, 32-byte Ed25519 key)`, an input is a bare outpoint, and the
  only spending rule is a signature by that key: Taproot's key path without the script path.
  Paying the key itself, rather than a hash of it, makes addresses longer (51 characters, not 34),
  and an input carries a 64-byte signature and no public key.
- **Signatures are a separate witness section**, one per input. `txid` hashes only the body, so
  nobody but the key holder can change a txid that others refer to (pre-SegWit Bitcoin was
  third-party malleable). `wtxid` hashes everything.
- **The merkle tree is over wtxids**, so the header commits to the signatures directly. SegWit had
  to bolt a witness commitment onto the coinbase only because it was a soft fork.
- **One sighash type**: every input signs `tagged_hash("BOUND/sighash", txid)`, which covers all
  inputs and outputs. It doesn't commit to the spent amounts the way BIP143/341 do for offline
  signers; that's a possible later improvement.
- **The coinbase has no inputs.** It's made unique by `lock_time = height`, the approach BIP54
  proposes for Bitcoin. Its memo carries the miner's extra nonce, and the genesis message.
- **The merkle tree is tagged, and an odd node is promoted rather than duplicated.** That removes
  CVE-2012-2459 and the 64-byte-transaction ambiguity.
- **Ed25519 consensus is libsodium's strict verify**: it requires canonical S and rejects
  small-order keys. `Ed25519.RejectsNonCanonicalS` and `Ed25519.RejectsSmallOrderPublicKey` pin
  this down. A lax verifier accepts both, and the second is a key whose coins anyone could spend.
