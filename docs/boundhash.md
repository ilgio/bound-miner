# BoundHash

BOUND's own proof-of-work hash, since [milestone 16](milestone-16.md). It replaced SHA-256d, the
hash Bitcoin mines.

## Why not SHA-256d

Bitcoin's network does hundreds of exahashes a second on machines (ASICs) that can only compute
SHA-256d. Any of them could mine a SHA-256d coin instead, and a sliver of that fleet is millions of
PCs' worth. On a new, small coin, one such machine would take nearly every block, and if it left,
blocks would stall until the difficulty fell back ([launch day](launch-day.md) had the numbers).

None of those machines can compute BoundHash. Mining it takes a CPU's own strengths: half a megabyte
of fast memory per hash, read and written at places that can't be known in advance, and 64-bit
multiplications.

## The algorithm

Words are 64-bit unsigned integers, read and written little-endian; `+` and `*` wrap mod 2^64;
`rotr` rotates right; `lo32(x)` is the low 32 bits. `L` is the scratchpad's size in 128-byte lines,
a power of two: **4,096 (512 KiB) on mainnet and testnet**, 64 on regtest (for quick tests).

`TaggedHash(tag, x) = SHA256(SHA256(tag) || SHA256(tag) || x)`, as elsewhere in BOUND.

**The round** works on 16 words `v[0..15]`. It's one round of BLAKE2b without message words, with
Argon2's BlaMka in place of its additions:

    BM(x, y)   = x + y + 2 * lo32(x) * lo32(y)
    G(a,b,c,d) : a = BM(a,b); d = rotr(d ^ a, 32); c = BM(c,d); b = rotr(b ^ c, 24)
                 a = BM(a,b); d = rotr(d ^ a, 16); c = BM(c,d); b = rotr(b ^ c, 63)
    R(v)       : G(0,4,8,12) G(1,5,9,13) G(2,6,10,14) G(3,7,11,15)
                 G(0,5,10,15) G(1,6,11,12) G(2,7,8,13) G(3,4,9,14)
    F(x, i)    = R(x with x[15] ^= i)  XOR  x

**1. Seed.** For the 80-byte header `H`: `seed = TaggedHash("BOUND/BoundHash", H)`. The starting
state `S` is 16 words: `SHA256(seed || k)` for the bytes k = 0, 1, 2, 3, each read as 4 words.

**2. Fill** the scratchpad `M[0..L-1]`:

    M[0] = F(S, 0)
    M[1] = F(M[0], 1)
    for i = 2 .. L-1:
        ref  = (lo32(M[i-1][0]) * (i - 1)) >> 32        // an earlier line, 0 .. i-2
        M[i] = F(M[i-1] XOR M[ref], i)

**3. Mix:**

    v = S XOR M[L-1]
    for r = 0 .. L-1:
        j = v[0] mod L
        v = v XOR M[j]
        (hi, lo) = v[0] * (v[1] | 1)                    // the full 128-bit product
        v[2] = v[2] + hi;  v[3] = v[3] ^ lo;  v[15] = v[15] ^ r
        v = R(v)
        M[j] = M[j] + v                                  // word by word

**4. Result:** `TaggedHash("BOUND/BoundHash-result", seed || v)`, with `v` as 128 bytes. It's read as
a 256-bit number the way block ids are (the last byte most significant), and a header's proof of
work is valid when that number is at most the target its `bits` encode.

The reference implementation is [contrib/boundhash/boundhash.py](../contrib/boundhash/boundhash.py),
plain Python that shares no code with the C++ ([src/core/boundhash.cpp](../src/core/boundhash.cpp)).
`tests/test_boundhash.cpp` checks that both give the same hashes, including at mainnet's size:

    python3 contrib/boundhash/boundhash.py --vectors

## Why each step

- **The whole header goes into the seed at once.** SHA-256d miners hash the first 64 bytes of a
  header once and then only the last 16 per nonce (the "midstate"); here each nonce costs a whole
  BoundHash, with nothing to precompute.
- **The fill picks earlier lines from the data**, as Argon2d does. A miner who keeps only some of
  the lines, to need less memory, must rebuild each missing one from the lines it came from, and
  theirs, so saving memory costs far more work.
- **The mix reads and rewrites lines in an order nobody can know in advance**: each step's line
  comes from the step before. A hash can't be split up to run in parallel, and since the lines keep
  changing, the scratchpad can't be kept in any shorter form.
- **The 64 × 64 → 128-bit multiply** is one instruction on a 64-bit CPU and several on a GPU, whose
  arithmetic is 32-bit.
- **SHA-256 at both ends.** The result is a hash of everything that came before it, so it's as evenly
  spread as SHA-256's. If the middle ever turned out to have a shortcut, a miner could take that
  shortcut and still have to search for a low hash nonce by nonce: a shortcut could make mining
  cheaper, never let anyone forge a block.
- **The tweaks** (`i` in the fill, `r` in the mix) make every step different, so no stretch of the
  computation repeats another.

## Where it's used

- **Only for proof of work.** A block's id stays `SHA-256d(header)`, as on Litecoin and Dogecoin,
  which kept theirs when they changed their proof of work: ids are needed everywhere and must be
  cheap. So ids no longer start with zeros. `boundd genesis` prints both.
- **Once per header**, when it enters the block tree or during headers presync. A block's body is
  checked without it (`check_block`'s `check_pow`, as Bitcoin's `CheckBlock` has).
- **Bits outside the network's range are refused before any hashing.**

## How fast

Measured on an Intel Core i5-10400F (6 cores, 12 threads, 12 MB of L3 cache), for the scratchpad
sizes considered:

| Scratchpad | One hash (one thread) | Most hashes a second, every thread |
|---|---|---|
| 2 MiB | ~4 ms | ~600 (stalled on RAM after 2-4 threads) |
| 1 MiB | ~1.7 ms | ~1,700 |
| **512 KiB** | **~0.7 ms** | **~5,400** |
| 256 KiB | ~0.4 ms | ~11,700 |

512 KiB is the balance: every node checks every header's proof of work, including the small
free-tier servers, and at 2 MiB that took several milliseconds on a desktop and far longer on a
shared cloud core; a miner still needs half a megabyte of fast memory beside every core it hashes on.

On that PC every core but one (the wallet app's default) does about 4,500 hashes a second. Mainnet's
difficulty 1 is 2^22 hashes a block, about a quarter of an hour there.

## What it doesn't promise

- **It isn't GPU-proof.** A graphics card has lots of fast memory and could probably outpace a PC once
  someone writes a miner for it, as on most coins. Only a design like Monero's RandomX, which runs
  random programs that need a whole CPU, is built to stop that, at many times the complexity.
- **It isn't ASIC-proof.** Anyone can build a machine for any algorithm. Memory per hash makes one
  costly to build, and there's no reason to until the coin is worth a lot.
- **It's new.** Its pieces (SHA-256, Argon2's round, Argon2d's way of filling memory) are well
  studied; their arrangement here isn't. Report anything you find through [SECURITY.md](../SECURITY.md).

Changing BoundHash is a hard fork: before mainnet launches, a new genesis; after, a switch at a
block height that every node upgrades for.
