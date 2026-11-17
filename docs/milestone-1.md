# Milestone 1: primitives, header, proof of work ✅

**What it adds:** hashing, the wire format, the 80-byte block header, proof of work, a miner, and
a linear chain of headers in memory.

## Files

| File | What it does | Tests |
|---|---|---|
| `util/hex.*` | hex encode/decode | `Hex.*` |
| `util/serialize.*` | little-endian `ByteWriter`/`ByteReader`, CompactSize (rejects non-canonical), length-capped `var_bytes` | `Serialize.*` |
| `crypto/sha256.*` | incremental SHA-256 via libsodium, `sha256d`, BIP340-style `tagged_hash` | `Sha256.*` |
| `core/uint256.*` | 32-byte little-endian number; display hex is reversed, as in Bitcoin | `Uint256.*` |
| `core/pow.*` | compact `bits` ↔ target (`SetCompact`/`GetCompact` semantics), `check_proof_of_work` | `Compact.*`, `Pow.*` |
| `core/block.*` | `BlockHeader`, byte-identical to Bitcoin's | `BlockHeader.*` |
| `core/miner.*` | `mine()`: nonce search that never wraps | `Miner.*` |
| `core/params.*` | testnet consensus parameters and the genesis block | `Params.*` |
| `core/chain.*` | `HeaderChain`: linear header chain; PoW, difficulty, median-time-past and future-time rules (milestone 4 replaced it with the block tree) | `BlockTreeTest.*` since milestone 4 |

The test vectors are independent of this code. They include Bitcoin's real genesis and block 1
headers (the code reproduces `000000000019d6…`), NIST SHA-256 vectors, and Bitcoin Core's
compact-target cases.

## Where BOUND differs from Bitcoin

- **The same as Bitcoin:** the 80-byte header, SHA-256d PoW, compact `bits`, little-endian
  `uint256`, median-time-past (11 blocks) and the 2-hour future limit. Like current Bitcoin Core,
  the "now" for that limit is the local clock (Core removed its network-adjusted time).
- **Tagged hashes** (the BIP340 construction) for every hash BOUND defines itself (txids,
  wtxids, sighash, merkle nodes), so different kinds of data can never hash to the same thing.
- **Fixed difficulty** (`pow_limit_bits = 0x1f00ffff`, about 65k hashes per block) until
  milestone 4 adds daily retargeting.
- **Linear chain only.** Forks, choosing the chain with the most cumulative work, and reorgs come
  in milestone 4.

## Possible speed-ups

- **Midstate:** the first 64 header bytes don't depend on the nonce. Hashing them once and copying
  the `Sha256` state per nonce saves one of the three SHA-256 compressions per attempt.
- **Multithreaded miner:** split the nonce range across `std::jthread`s.
