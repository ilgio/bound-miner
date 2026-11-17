# Developer fee

bound-miner keeps **5% of every block it mines on mainnet** for its developer, ilgio.
It is how the GPU work is paid for.

| | |
|---|---|
| Share | 5% of the block's whole reward (subsidy plus fees), rounded down |
| Paid to | `BW2WEtNxrj1jBkydkG9qWdDGZkyfZjcdx61CEWfEsCrz8aheLdVz` |
| Where | mainnet only; testnet and regtest are never charged |
| Who pays | any block mined by this build, on a GPU or a CPU |

## How it works

When your node builds a block to mine, its coinbase has **two outputs**: the first pays your address
the other 95%, the second pays the developer's address 5%. That is the whole mechanism. It is an
ordinary coinbase that consensus accepts, so the block is valid on the network like any other; the
share is paid at the moment the block is found. Nothing is held back, there is no pool and no hidden
connection.

- **Announced:** the splash at startup shows it, and the log says
  `developer fee: 5% of each block mined here goes to ...`.
- **Reported:** `./bound-cli getmininginfo` shows `devfee_percent` and `devfee_address`.
- **Verifiable:** every block you mine shows both outputs in a block explorer: the second is exactly
  `floor(reward * 5 / 100)`.
- **Not applied** if the address you mine to is the developer's own.

## If you don't want it

The official BOUND client (<https://github.com/Sonic-Bound/Bound>) has no fee but mines only on CPUs.
