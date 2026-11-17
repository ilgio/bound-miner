# Milestone 16: BoundHash

**What it adds:** BOUND's own proof of work, [BoundHash](boundhash.md), in place of Bitcoin's
SHA-256d, on every network. The testnet starts over on it.

## Decided (2026-11-16, by the project owner)

"Make BOUND's own hashing technique." The risk it answers was in the [launch-day](launch-day.md)
notes: with SHA-256d, one of Bitcoin's mining machines could take nearly every early block, then
leave the chain stalled. BoundHash can't be mined on them.

Claude's choices within that, explained in [boundhash.md](boundhash.md):
- **Built from studied parts:** SHA-256 at both ends, Argon2's mixing round, Argon2d's way of
  filling memory. Only the arrangement is new, and the SHA-256 at the end keeps it a sound proof of
  work whatever the middle turns out to be.
- **512 KiB per hash.** 2 MiB was the first try: on a 6-core desktop it stalled on RAM at about 600
  hashes a second, and took ~4 ms to check one header. 512 KiB stays in the caches, ~5,400 a
  second, 0.7 ms to check.
- **The block id stays SHA-256d.** Only proof of work changes (as when Litecoin chose scrypt), so ids,
  links between blocks and everything that stores them are as before.
- **Mainnet's difficulty 1 is 2^22 hashes** (`0x1e03ffff`), not Bitcoin's 2^32: what a PC on every
  core finds in about a quarter of an hour, which is what Bitcoin's difficulty 1 cost a PC.
  ASERT still never goes below it.
- **The testnet restarts:** the same genesis message and date (01/Nov/2026), mined again with
  BoundHash, and a new magic so old and new nodes drop each other at once.

## In the code

- `core/boundhash.{h,cpp}`: the hash. The round runs on 16 local variables, not an array: MSVC kept
  the array in memory and ran at half the speed.
- `contrib/boundhash/boundhash.py`: a reference implementation in plain Python. The test vectors come
  from it, so two implementations that share no code agree.
- `core/pow.h`: `pow_hash(header, params)` and `check_proof_of_work(header, params)`, which refuses
  bits outside the network's range before hashing. Every check of a header (block tree, headers
  presync, blocks) goes through it.
- `ConsensusParams::pow_lines`: the scratchpad's size. 4,096 lines (512 KiB) on mainnet and testnet,
  64 on regtest, so tests that mine thousands of blocks stay quick.
- **Checked once:** `check_block(block, params, check_pow)`. A block whose header is already in the
  tree, and every block connected to the chain, skips proof of work, which its header passed. Before,
  a new block was hashed three times.
- **The miner** has no midstate any more (BoundHash takes the whole header at once); each thread keeps
  its scratchpad, and looks for new work every 64 hashes instead of every 65,536.
- **`boundd genesis`** hands out 256 nonces at a time and prints both the id and the BoundHash.
- **A chain from a replaced genesis is set aside.** `boundd node` and bound-qt move it, with its
  indexes and wallet, to `old-chain-<its genesis id>` in the data directory, say so in the log, and
  start the new chain. That's the old testnet after this upgrade, and anything from mainnet's
  provisional genesis after launch.

## The testnet, started over

| | Before | Now |
|---|---|---|
| Proof of work | SHA-256d | BoundHash, 512 KiB |
| Genesis | `0000a7ba…9293` (nonce 26,584) | `019fd113…e8d1` (nonce 38,222), same message and time |
| Magic | `b0 d4 e7 f5` | `b3 d8 ea f1` |
| `minimum_chain_work` | the old chain at height 7,000 | 0 again, until the new chain has grown |
| Easiest target | `0x1f00ffff`, ~65,000 hashes | the same: ~15 seconds for a 6-core PC |

Everything else (ports, addresses, economics, seeds) is unchanged. The old chain's coins had no
value and don't carry over.

## Tests

- **BoundHash:** both implementations agree on 12 vectors (4 sizes, up to 2 MiB); every bit of
  the header changes the hash; one answer on any thread and after other sizes; the fast 128-bit
  multiply agrees with a portable one; bad sizes are refused.
- **Proof of work** checks BoundHash and not the id: a test finds headers that pass by one and fail by
  the other.
- **The miner and the genesis search** find the first valid nonce (computed by the Python too), the
  same on any number of threads, and move the time on when a second has no solution.
- **Setting a chain aside** moves it and its wallet, leaves another network's chain or this genesis's
  own alone, and the old chain still opens from where it was put.
- `Networks.EveryGenesisIsValid` checks each network's genesis against BoundHash.
