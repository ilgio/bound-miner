# Security policy

BOUND is a cryptocurrency, so a bug can cost people money. If you find one that could:
- let someone spend coins that aren't theirs, create coins, or break a consensus rule;
- crash, stall or split nodes;
- leak a wallet's seed or passphrase;

then **please report it privately first**.

## How to report

Use GitHub's private vulnerability reporting: on this repository, **Security → Report a
vulnerability**. Only the maintainers see the report. Please include:

- what the problem is and what an attacker gains;
- the BOUND version (the commit, or `bound-cli getnetworkinfo` → `subversion`) and the network
  (mainnet, testnet, regtest);
- steps or an input that reproduce it. For a crash found by fuzzing, the input file `bound_fuzz`
  saved.

You'll get an answer within a few days. Once a fix is released, the report is published with
credit to you, unless you'd rather stay anonymous.

Bugs that can't hurt anyone (typos, crashes of tools on your own machine with bad command-line
arguments) can go straight to the public issue tracker.

## What's in scope

- `boundd` and everything it runs: consensus, peer-to-peer networking, the mempool, the wallet,
  JSON-RPC.
- `bound-qt`, `bound-cli`, `bound-explorer` and `bound-seeder`.
- The build and release process, once releases exist (milestone 13).

## What's done to find bugs first

Every push runs the tests on Linux and Windows, and again under three sanitizers:
AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer. Every decoder and the peer
message handler are also fuzzed with libFuzzer. See
[docs/milestone-12.md](docs/milestone-12.md) and
[docs/consensus-review.md](docs/consensus-review.md).

## Supported versions

Only the latest commit on `main` gets fixes until mainnet launches. After that, the latest release
does.
