# Milestone 15: the launch

**What it adds:** everything mainnet needs, short of the day itself:
- the starting difficulty;
- the seeds;
- a tool that mines the genesis;
- a [launch-day runbook](launch-day.md).

What's left is that day's work: its headline goes into the genesis, then the release and the
servers.

## Decided (2026-11-16, by the project owner)

- **"The exact way Bitcoin did."** A genesis that quotes a headline from launch day, no premine,
  the program published and then mined by anyone, the project owner included, from block 1.
- **Bitcoin's exact difficulty 1.** Mainnet's `pow_limit_bits` is `0x1d00ffff`, Bitcoin's, and
  the genesis starts there:
  - **Cost:** about 2^32 hashes a block, roughly 16 minutes for one PC on every core (4.5 million
    hashes a second).
  - **Floor:** ASERT raises the difficulty as miners join and never lowers it below 1. If nobody
    mines, the chain waits.
  - **Instead of:** milestone 11's plan, an easier floor with the genesis's difficulty fitted to the
    expected hash rate.
  - **Side effect:** ASERT's arithmetic is now inside the specification's own domain (a limit below
    2^224), the one Bitcoin Cash runs. The extension milestone 11 built for an easier limit is no
    longer reached on mainnet, and stays tested.
- **As soon as it's ready.** No date announced ahead: the release is the announcement.

## Mining a genesis: `boundd genesis`

    boundd --mainnet genesis "BOUND 20/Nov/2026 <a headline from that day>" now

It mines on every core and prints the lines that go into `params.cpp`. It used to search one thread,
one second's nonces, and give up. A search of about 2^32 hashes needs more:
- **Every core.** The work is cut into chunks of 2^20 nonces, handed out in order.
- **The time moves on.** At difficulty 1 a given second has a solution only about two times in three.
  When none of its 2^32 nonces works, the search goes on to the next second.
- **Always the same answer.** The first solution in (time, nonce) order, however many threads, so
  anyone can check a genesis by mining it again. Without arguments it does just that for the
  network's own genesis: `boundd --testnet genesis` prints the testnet's, nonce 26584, in a second.

`core/genesis.h` has the two halves:
- `make_genesis`: the unmined block, Bitcoin's kind of coinbase (the reward to a key nobody holds,
  the message as its memo);
- `mine_genesis`: the search.

Four tests check them. One mines the testnet's genesis again on one thread and on eight. Another
shrinks a second to 1,000 nonces so that the search has to move the time on, and then checks that no
earlier (time, nonce) worked.

The provisional mainnet genesis was mined again at difficulty 1 with it. Launch day replaces it.

## Seeds

**Mainnet** (`p.seeds`):
- `seed.bound.sarl`, `seed2.bound.sarl`: the DNS seeders;
- `136.86.151.244`, `104.211.211.39`: the Google Cloud and Azure nodes, in case DNS fails.

**Testnet:**
- **Seeds:** the two nodes' addresses only.
- **Why only those:** a seeder holds its machine's DNS port, so each machine can seed one network.
  On launch day both seeders move to mainnet.
- **What changed in the tools:** `bound-server seeder` now stops another network's seeder before it
  starts one. `bound-server explorer` refuses a name another network's explorer has: testnet's
  explorer moves to `testnet-explorer.bound.sarl` first, then mainnet's takes `explorer.bound.sarl`.

**Testnet's `minimum_chain_work`** is now the public chain's work at height 7,000
(`0x1b201b200`). A fresh node no longer stores headers from a chain with less work. That was 0
until there was a public chain ([hardening](hardening.md)).

## Ready for the switch

`kDefaultNetwork` decides which network a program joins without `--mainnet`/`--testnet`. Every
usage text now names it rather than saying "testnet", and so does the wallet's About box (mainnet
"from launch day" until it has launched). Launch day changes one line for that.

## Files

| File | What it does |
|---|---|
| `core/genesis.*` | `make_genesis`, `mine_genesis` |
| `app/main.cpp` | `boundd genesis`: every core, the time moves on, prints `params.cpp` lines |
| `core/params.cpp` | mainnet: difficulty 1, seeds, provisional genesis re-mined; testnet: seeds, `minimum_chain_work` |
| `contrib/deploy/bound-server` | one seeder per machine; explorer names can't clash |
| `docs/launch-day.md` | the runbook |
| `tests/test_genesis.cpp` | the four tests above |
