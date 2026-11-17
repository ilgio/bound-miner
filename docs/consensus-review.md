# Consensus review

A line-by-line read of BOUND's consensus code before mainnet (milestone 12). It works through the
classes of bug that have hit real coins.

**Verdict:** no consensus bug found. One risk needs watching (Ed25519 verification rules), and is
now guarded by a version requirement and by tests.

**Files read:**
- `core/consensus.cpp`: context-free rules;
- `core/validation.cpp`: inputs, connecting blocks, the block tree;
- `core/merkle.cpp`;
- `core/pow.cpp`: proof of work, retarget, ASERT;
- `core/transaction.cpp`, `core/block.cpp`: serialization;
- `crypto/ed25519.cpp`;
- `wallet/crypter.cpp`.

## Checked, and sound

| Class of bug | Real-world case | In BOUND |
|---|---|---|
| **Value overflow** | Bitcoin 2010: 184 billion BTC | Every output, every running total of outputs and of inputs is checked against `kMaxMoney` before it can overflow. Fees are each ≤ their inputs, so their sum fits; the coinbase is checked against subsidy + fees. |
| **Merkle tree duplication** | Bitcoin CVE-2012-2459: a block and a mutated copy with duplicated transactions share a merkle root, and the copy gets the real block blacklisted | An odd node is carried up a level, not duplicated, so no two lists of transactions share a root. And a body failing the context-free rules is refused *without* marking its header invalid (`accept_block`), so a mutated body can never blacklist a block. |
| **Inner node posing as a transaction** | Bitcoin's 64-byte transaction weakness | Inner nodes are hashed under the tag `BOUND/merkle` and leaves are wtxids under `BOUND/wtxid`: different domains, no confusion. |
| **Transaction malleability** | Mt. Gox era: txids changed in flight | Signatures are outside the txid (in the witness, covered by the wtxid only). Encodings are canonical: fuzzing confirmed every transaction and block that decodes re-encodes to exactly the same bytes. libsodium rejects non-canonical S, so even wtxids can't be malleated by third parties. |
| **Duplicate transactions** | Bitcoin BIP30/BIP34 | A coinbase's lock time is its height (checked when connecting), so coinbase txids are unique. Other transactions spend distinct inputs. Creating an output that already exists is refused (`DuplicateOutput`). |
| **Inflation through the coinbase** | | The coinbase may claim at most subsidy + fees. Its outputs mature after 100 blocks. The genesis output never enters the UTXO set. |
| **Timewarp** | Bitcoin's off-by-one (BIP54), Verge 2018 | Testnet: each retarget window starts at the previous window's last block, the Litecoin fix. Mainnet: ASERT follows an absolute schedule, so there's no window to game, and a timestamp can't run more than 2 hours ahead of the honest nodes' clocks. |
| **Time rules** | | A block's time must exceed the median of the last 11 and be no more than 2 hours ahead of the receiving node's clock. Timestamps are 32-bit unsigned, good until 2106. |
| **Difficulty arithmetic** | | ASERT matches Bitcoin Cash's test vectors exactly ([milestone 11](milestone-11.md)). Its range errors can't be reached from a header: a 32-bit time and a 31-bit height keep the schedule lag under 2^47. The retarget is exact (`mul_div`) and clamped. |
| **Resource limits** | | Blocks are at most 1 MB, and messages at most 4 MB before decoding. Counts are checked against the bytes left before anything is allocated. Each input costs one Ed25519 verification, at most ~10,000 a block. Low-work header floods are presynced before storing ([hardening](hardening.md)). |
| **Signature scope** | | One sighash, covering the whole transaction body. Each input's signature commits to every input and output. |

## A risk to keep watching: Ed25519 verification rules

Consensus depends on exactly which signatures libsodium accepts. Ed25519 libraries, and versions of
the same library, have disagreed on edge cases:
- **small-order keys and R values:** the identity point "verifies" any message;
- **non-canonical S;**
- **cofactored versus cofactorless verification.**

Zcash had to write ZIP 215 because of this. Two nodes that disagree on one signature split the
chain.

**Guarded by:**
- **A minimum version.** Builds against a system libsodium now need 1.0.18 or newer (CMake). Windows
  builds get the version vcpkg's baseline pins.
- **Tests:** `Ed25519.RejectsNonCanonicalS` and `Ed25519.RejectsSmallOrderPublicKey`. The latter
  forges a signature with the identity point that a lax verifier would accept. A libsodium that
  behaved differently fails the test suite before it can ship.
- **Rule:** never swap the Ed25519 library, or change how verification is called, without running
  those tests, and preferably a full set of edge-case vectors (ZIP 215's) against both old and new.

## Wallet

- **Seed encryption:** Argon2id (64 MiB, 2 passes; above OWASP's minimum) and
  XChaCha20-Poly1305, with the derived key wiped after use. A higher setting (libsodium's
  "moderate", 256 MiB) would cost low-memory machines. It can be raised later for new wallets,
  since each wallet file records its own settings.
- **The KDF settings in a wallet file are bounded** (at most 4 GiB of memory), so a doctored
  wallet file can at worst make its own unlock slow.

## Not consensus, noted for later

- **Signing doesn't commit to the amounts being spent** (Bitcoin fixed that with segwit, BIP143).
  A hardware wallet signing a BOUND transaction would have to trust the amounts it's told, or check
  them against the earlier transactions. That's worth adding before hardware wallet support. It's
  a new signature hash, so it needs a version bump of transactions.
