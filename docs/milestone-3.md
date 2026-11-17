# Milestone 3: the UTXO set and full validation ✅

**What it adds:** the ledger itself. BOUND now knows who owns which coins, and enforces the rules
that need that knowledge:
- inputs must exist and be unspent, which stops double spends, including two inside one block;
- signatures must be by the spent output's owner;
- inputs must cover outputs;
- the coinbase may claim at most `subsidy + fees`;
- the coinbase must carry its height;
- coinbase outputs wait 100 blocks before they can be spent.

Every block can be undone exactly, which is what milestone 4's reorgs need.

## Files

| File | What it does | Bitcoin Core equivalent | Tests |
|---|---|---|---|
| `core/coins.*` | `Coin`, `UtxoSet`, `BlockUndo` | `coins.h`, `undo.h` | `UtxoSet.*` |
| `core/validation.*` | `check_tx_inputs`, `connect_block`, `disconnect_block`, `Chainstate` | `consensus/tx_verify.cpp`, `validation.cpp` | `ValidationTest.*`, `ChainstateTest.*` |
| `core/miner.*` | `make_block_template`, `mine_block` (with extra nonce) | `node/miner.cpp` | `ChainstateTest.Template*` |
| `core/wallet.*` | `balance`, `spendable_coins`, `create_payment` (minimal single-key wallet) | `wallet/` (tiny subset) | `WalletTest.*` |
| `core/chain.*` | adds `check()` (validate without appending) and `pop_tip()`; both gone with milestone 4's block tree | | |

## Rules (chosen by the project owner)

- **Coinbase maturity: 100 blocks**, the same count as Bitcoin; about 100 minutes at one block
  per minute. The coinbase from block 1 can first be spent in block 101.
- **The genesis reward is unspendable**, as in Bitcoin. Its output never enters the UTXO set, and
  it pays the all-zero key, which nobody can sign for.

## How a block is accepted

This is the milestone 3 version, on a single chain. Milestone 4 accepts blocks on any branch;
see [its notes](milestone-4.md).

`Chainstate::accept_block(block, now)`:
1. `HeaderChain::check`: proof of work, it extends the tip, difficulty, time rules.
2. `connect_block`:
   - `check_block` (the context-free rules);
   - the coinbase height must match;
   - then each transaction in order: `check_tx_inputs` (exists → mature → value range → inputs ≥
     outputs → signatures), spend its inputs, add its outputs;
   - finally, the coinbase must claim no more than subsidy + fees.

   Any failure rolls back every change made so far.
3. Only if both pass: append the header, and store the block and its undo data.

`disconnect_tip()` walks the block backwards (last transaction first): it removes each
transaction's outputs and restores the coins it spent from the undo data. Walking backwards is
what makes chains of transactions inside one block undo correctly. `ValidationTest.CanSpendAnOutputFromEarlierInTheSameBlock`
pins this down.

## Try it

`boundd demo`. Use the Release build: it mines at about 430k hashes/s, roughly 10× the Debug
build. The demo mines 101 blocks, shows that only the 100-block-deep coinbases can be spent, has
Alice pay Bob, checks that supply equals blocks × 5 BOUND, then disconnects the last block and
shows Bob's payment disappear.
