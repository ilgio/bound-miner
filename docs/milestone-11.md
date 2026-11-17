# Milestone 11: ASERT difficulty ✅

**What it adds:** mainnet sets every block's difficulty with **aserti3-2d**, Bitcoin Cash's
algorithm since November 2020. It was the project owner's choice over Bitcoin's daily retarget,
which testnet keeps.

How it works:
- **The schedule.** The genesis time plus 10 minutes for every block since.
- **The rule.** For every 2 days (the half-life) the chain is *behind* that schedule, the target
  doubles and blocks get easier. For every 2 days it's *ahead*, the target halves and blocks get
  harder. In between it scales smoothly, block by block.
- **What that fixes.** A miner arriving can't race through a day of cheap blocks, and one leaving
  can't strand the chain at a difficulty nobody can meet. The difficulty depends only on the
  parent's height and time, so there's no window to game.

## Files

| File | What it does | Bitcoin Cash equivalent | Tests |
|---|---|---|---|
| `core/pow.*` | `asert_target`; `next_work_required` uses it when `asert_half_life` is set | `src/pow/aserti32d.cpp` (`CalculateASERT`, `GetNextASERTWorkRequired`) | `Asert.*` |
| `core/params.*` | `asert_half_life` (mainnet: 2 days; testnet, regtest: off) | `nASERTHalfLife` | `Asert.IsMainnetsRule` |
| `tests/data/asert/run01…run12` | the official test vectors, unchanged (MIT, from Bitcoin Cash Node's qa-assets) | | `Asert.MatchesBitcoinCashsTestVectors` |

## Choices

- **The anchor is the genesis block.** It has no parent, so its own time stands in for the
  parent's, as the specification has it for a genesis anchor. As a result the first block's
  target is 2^(-600/172,800), 0.24% harder than the genesis block's.
- **The arithmetic.** It follows the specification line for line, with one exception.
  - **The exception:** the anchor target × the 2^x factor is computed in a single exact
    multiply-and-divide (`mul_div`, the step testnet's retarget already uses) rather than in
    256 bits.
  - **Where the specification applies** (an easiest target below 2^224), the result is identical.
    Dividing by 2^a and then by 2^b gives the same as dividing by 2^(a+b), and `mul_div` is exact.
  - **Above that range** it stays exact where the specification's own arithmetic would overflow.
- **Why that matters.** It lets mainnet keep an easy floor: about 65,000 hashes a block, the same
  as testnet's.
  - With the specification's limit, the floor would be Bitcoin's difficulty 1: 4.3 billion hashes,
    about 1.8 hours a block for one thread of today's miner.
  - On a young, CPU-mined network, that floor would leave blocks slower than 10 minutes whenever
    few people mine, and ASERT couldn't correct it.
  - With an easy floor, ASERT can follow the hash rate wherever it goes.
- **Where launch starts.** It's the genesis block's bits, chosen on launch day to fit the expected
  hash rate (milestone 15).
  - The provisional genesis sits at the floor, so a preview chain mines its first blocks
    instantly, then gets harder as it runs ahead of schedule.
  - A real launch must start near the real hash rate. Otherwise the first miners would get the
    blocks it takes ASERT to catch up, roughly 288 blocks for every doubling of difficulty.

## Testing

- **The official vectors.** All 12 files of Bitcoin Cash's test vectors match, nBits for nBits.
  That's 13,000+ cases, covering:
  - the minimum and maximum targets;
  - heights past 2^31 and 2^63, and times past 2^31;
  - timestamps running backwards;
  - random solve times with steady, rising and falling hash rates.
- **The reference code.** A line-for-line copy of the specification's C++ in the test, compared
  with `asert_target` on 200,000 random anchors, heights and times. Half of those are far off
  schedule, where the clamps take over.
- **Beyond the specification** (an easy limit): one half-life behind doubles the target exactly
  and one ahead halves it. The result never decreases as time passes, the clamps at the limit and
  at 1 hold, and bad input is refused.
- **On chains with ASERT rules:**
  - steady blocks keep a steady target;
  - a burst of fast blocks makes each one harder, and a long gap brings it back to the floor;
  - every block's bits are exactly `asert_target`'s for its parent;
  - a block carrying its parent's bits, or the limit's, is refused even with valid proof of work.
- **Mutation check.** Five deliberate breakages of `asert_target` were each caught:
  - a cubic coefficient off by one;
  - the missing one-block offset;
  - scaling off by a factor of 2;
  - a floor of 2 instead of 1;
  - no rounding in the cubic.
- **Live.** A mainnet preview node mined 199 blocks in 45 seconds.
  - Its difficulty stayed at the floor while the chain caught up with its schedule, then rose
    every block.
  - At 138 blocks ahead of schedule it showed 1.397, where the formula gives 2^(83,400/172,800) =
    1.398.
- **Totals:** 399 tests pass on Windows (MSVC) and Linux (GCC 13).
