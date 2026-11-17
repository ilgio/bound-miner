# The road to mainnet

Mainnet is the network where BOUND is meant to have value. Its consensus rules can't be taken back
once people hold coins on it, so every rule is settled, tested and reviewed before launch day. The
testnet keeps running beside it, as Bitcoin's does.

## Decided (2026-11-15, by the project owner)

| | Mainnet | Testnet (unchanged) |
|---|---|---|
| Block time | 10 minutes | 1 minute |
| Reward | 50 BOUND, halving every 210,000 blocks (~4 years) | 5 BOUND, halving every 2,100,000 blocks |
| Supply | 20,999,999.9769 BOUND, Bitcoin's exact schedule | 20,999,999.727 BOUND |
| Premine | none | none |
| Proof of work | [BoundHash](boundhash.md), BOUND's own (milestone 16) | the same |
| Difficulty | ASERT, adjusted every block (Bitcoin Cash's aserti3-2d), from difficulty 1 (2^22 BoundHash hashes), never lower | Bitcoin's rule: daily, at most 4× |
| Addresses | start with `B` (2-byte version, 52 characters) | start with `9` |
| Default network | mainnet since launch day (16/Nov/2026); `--testnet` for testnet | |

Kept from the testnet: Ed25519 pay-to-key outputs, 1 MB blocks, coinbase maturity of 100 blocks,
timestamps at most 2 hours ahead, the timewarp-proof time rules, full RBF.

## Milestones

| # | Milestone | What it takes | Status |
|---|---|---|---|
| 10 | **Networks** | mainnet, testnet and regtest in one program (`--mainnet`, `--testnet`, `--regtest`), each with its own parameters, magic bytes, ports, address prefix and data directory; a provisional mainnet genesis | ✅ done ([notes](milestone-10.md)) |
| 11 | **ASERT difficulty** | aserti3-2d in integer math, exactly as specified, checked against Bitcoin Cash's published test vectors; mainnet only | ✅ done ([notes](milestone-11.md)) |
| 12 | **Hardening for real value** | fuzz tests for every parser (messages, blocks, transactions, addresses, HTTP/RPC); sanitizer builds (ASan, UBSan) in CI; Windows CI; a consensus review checklist (overflow, malleability, time rules, resource limits); wallet review; `SECURITY.md` and a private way to report bugs; a faster miner (SHA-256 midstate, one thread per core: today's does ~650,000 hashes a second on one thread) | ✅ done ([notes](milestone-12.md), [review](consensus-review.md)) |
| 13 | **Releases** | versioned GitHub releases: a Windows zip with the Qt files and a Linux tarball, SHA-256 checksums, signed; release notes | ✅ done ([notes](milestone-13.md), [how](release-process.md)) |
| 14 | **Launch infrastructure** | at least 3 always-on nodes with different providers and regions; a domain with a DNS seeder; a public explorer over HTTPS; monitoring | tools ✅ ([notes](milestone-14.md), [guide](running-a-server.md)); servers: 2 of 3 up (Google Cloud, Azure), `bound.sarl` with the explorer and 2 DNS seeders; still to do: a third provider, monitoring |
| 15 | **Launch** | the final genesis, its message quoting a headline from launch day so nobody can have mined before it (that's how Bitcoin showed it had no premine); difficulty 1 to start (the project owner's choice); the release as the announcement; the default network flipped to mainnet | launched 16/Nov/2026 ✅ ([notes](milestone-15.md), [launch day](launch-day.md)) |
| 16 | **BoundHash** | BOUND's own proof of work (the project owner's choice), so Bitcoin's mining machines can't take over the young chain: memory-hard, built from SHA-256 and Argon2's mixing, with a reference implementation and test vectors; the testnet started over on it | ✅ ([notes](milestone-16.md), [spec](boundhash.md)) |

After launch, each release raises `minimum_chain_work` (and could add an assumed-valid block) so
that new nodes can't be fed a cheap fake chain.

## Outside the code

- **Law and tax.** Before BOUND trades for money, get advice where you live. In India, crypto
  gains are taxed at 30% with 1% TDS on transfers, and services that exchange or hold crypto must
  register with FIU-IND.
- **Wallets and exchanges.**
  - **Trust Wallet** needs a `wallet-core` integration: addresses and signing, in C++, as a pull
    request.
  - **MetaMask** only takes non-Ethereum chains through a reviewed Snap.
  - **Exchanges** list coins with real use, a community, audits and a team they can identify.
  - All three come after a mainnet has run for a while.

## Why ASERT and not Bitcoin's rule on mainnet

Bitcoin's rule sets the difficulty once per period, from that period's speed. On a young network
whose hash rate can jump tenfold in an hour, that goes wrong both ways:
- **A big miner arriving** gets the rest of the period at the old, low difficulty: a day's blocks
  in minutes.
- **A big miner leaving** strands the others at a difficulty they can't meet, for days.

ASERT adjusts after every block, by how far the chain is ahead of or behind its schedule.
Difficulty doubles or halves for every 2 days the chain is off schedule, so it settles within
hours and can't be gamed by timestamps. Bitcoin Cash has run it since November 2020.
