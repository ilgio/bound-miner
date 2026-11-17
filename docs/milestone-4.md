# Milestone 4: difficulty, the block tree, most work wins ✅

**What it adds:** the rules for competing chains.
- Difficulty adjusts once a day, aiming for one block a minute.
- Every valid header joins a tree, so forks are kept instead of rejected.
- The node follows the valid chain with the most work, and reorganises when another branch overtakes it.
- A block that fails to connect, and everything built on it, is remembered as invalid.
- Headers can arrive before their blocks, ready for milestone 6's headers-first sync.

## Files

| File | What it does | Bitcoin Core equivalent | Tests |
|---|---|---|---|
| `core/arith_uint256.*` | 256-bit arithmetic: `+ - /`, shifts, comparisons, exact `mul_div` | `arith_uint256.h` | `Arith256.*` |
| `core/pow.*` | adds `block_proof`, `retarget`, `next_work_required`, `difficulty` | `pow.cpp`, `GetBlockProof` | `BlockProof.*`, `Retarget.*`, `NextWorkTest.*`, `Difficulty.*` |
| `core/chain.*` | `BlockIndex`, a node of the block tree; replaces milestone 1's `HeaderChain` | `chain.h` (`CBlockIndex`) | `BlockTreeTest.*` |
| `core/validation.*` | `Chainstate` rebuilt on the tree: `accept_header`, `accept_block`, fork choice, reorgs | `validation.cpp` (`AcceptBlockHeader`, `ActivateBestChain`) | `ReorgTest.*`, `ChainstateTest.*` |
| `core/miner.*` | templates carry the required difficulty | `node/miner.cpp` | `ChainstateTest.Template*` |

The retarget vectors include Bitcoin Core's own `pow_tests` cases. The other expected values were
computed independently with Python's arbitrary-precision integers.

## Difficulty (chosen by the project owner)

Bitcoin's rule, with a one-day period instead of two weeks:
- Every **1,440 blocks**, compare how long the last 1,440 blocks took with 1,440 minutes, and
  scale the target by that ratio.
- A retarget changes the difficulty by at most **4×** either way, and never makes it easier than
  the minimum (`pow_limit`, about 65k hashes per block).
- Every other block must have exactly its parent's difficulty.
- A block's timestamp may be at most **2 hours** ahead of the node's clock, as in Bitcoin.

**One deliberate difference from Bitcoin: the measuring window.**
- Bitcoin measures from the first to the last block of a period: 2015 gaps for 2016 blocks.
- The gap *between* periods is never measured, and the **timewarp attack** exploits it. A
  majority miner stamps each period's first block as early as the rules allow, so every period
  looks longer than it was. That lowers the difficulty without limit.
- Bitcoin can only patch this with a soft fork. BIP54 requires a period's first block to be at
  most 2 hours earlier than the block before it.
- A new chain can fix the window itself, as Litecoin did. Each BOUND window starts at the last
  block of the previous period. Consecutive windows share an endpoint, so together they measure
  exactly the chain's real elapsed time. (The first retarget measures from genesis: 1,439 gaps,
  compared against 1,439 minutes.)
- `NextWorkTest.WindowsShareEndpointsSoTimewarpGainsNothing` pins this down: the same timestamps
  measured Bitcoin's way would have made difficulty easier.

**What this rule means in practice:**
- **Starting from the minimum is slow.** One retarget raises difficulty at most 4×. A single CPU
  at about 430k hashes/s needs roughly 400× the minimum for one-minute blocks. That takes about
  five retargets, roughly a day of continuous mining.
- **The first retarget counts from the genesis timestamp.** Its window runs from genesis
  (1 Nov 2026, 00:00 UTC) to block 1439. A chain whose mining starts more than a day later sees
  a first period that looks too slow, so #1440 keeps the minimum and the climb starts at #2880.
  Bitcoin's first retarget worked the same way (genesis dated 3 Jan 2009, block 1 mined on
  9 Jan). A genesis re-mined at launch time would avoid it, an option for milestone 8.
- **A drop in hash rate keeps blocks slow until the next retarget.** If most of the hash rate
  leaves right after a retarget, the 1,440 blocks before the next one come in slowly, possibly for
  days.
- **No minimum-difficulty exception.** Bitcoin's testnet adds one (a block may use minimum
  difficulty after 20 minutes without one), but it causes "block storms" there. BOUND leaves it
  out.

## Fork choice

- **Chain work.** A block's proof is `2^256 / (target + 1)`, the expected number of hashes needed
  to find it (`block_proof`). A chain's work is the sum over its blocks.
- **The most work wins, not the most blocks.** The tip is the valid block with the most chain
  work. In `ReorgWork.MostWorkBeatsMostBlocks`, five hard blocks beat eight easy ones.
- **Ties go to the first seen.** Of two chains with equal work, the one that was complete first
  stays, as in Bitcoin.
- **No reorg depth limit and no checkpoints**, also as in Bitcoin.

## How a block is accepted

`Chainstate::accept_block(block, now)`:
1. **Header rules,** checked against the block's parent wherever it is in the tree, not just the
   tip:
   - proof of work;
   - the parent is known and not invalid;
   - `bits == next_work_required(parent)`;
   - time is after the parent's median time past, and at most 2 hours ahead of now.
2. **`check_block`.** A failure rejects the body without marking the header invalid. The
   transactions may just not be the ones the header commits to, and anyone can pair a valid
   header with garbage (`ReorgTest.MismatchedBodyDoesNotTaintTheHeader`).
3. **Store the block.** It becomes *linked* once it and all its ancestors have their transactions.
4. **`activate_best_chain`.**
   - Take the linked, valid block with the most work.
   - Disconnect back to where its branch leaves the active chain, using milestone 3's undo data,
     then connect forward along the branch.
   - If a block fails to connect, it and its descendants are marked invalid, and the loop picks
     again. That can lead straight back to the old chain
     (`ReorgTest.InvalidBlockOnABranchSendsTheTipBack`).

`accept_header` runs step 1 alone. A header that's merely too far in the future isn't remembered
as bad, so it can be sent again later.

## Try it

- `boundd demo` now ends with a reorg:
  1. Alice's block #102 contains her payment to Bob.
  2. Meanwhile Carol, cut off from Alice, mines two blocks on #101.
  3. When they reconnect, Carol's branch has more work, and Alice's node switches to it.
  4. Bob's payment is no longer in the chain. Since milestone 7, Alice's mempool puts it back in line
     and her next block confirms it again.
- `boundd mine 2885` (Release build, about 6 minutes) crosses two retargets:
  - At #1440 the difficulty stays at 1.00, because the first window starts at the genesis
    timestamp, days before the run.
  - At #2880 it jumps to 4.00, the maximum step. That window covers only the run itself, where a
    block takes a fraction of a second instead of a minute.
