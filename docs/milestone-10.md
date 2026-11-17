# Milestone 10: networks ✅

**What it adds:** mainnet, testnet and regtest in the same programs, as in Bitcoin. It's the
first step on the [road to mainnet](mainnet-roadmap.md).
- **`--mainnet`, `--testnet`, `--regtest`** for `boundd`, `bound-cli`, `bound-qt`,
  `bound-explorer` and `bound-seeder`. Testnet stays the default until launch day.
- **Each network has its own** parameters, P2P magic bytes, ports, address prefix, genesis block
  and data directory (`%APPDATA%\BOUND\mainnet`, `\testnet`, `\regtest`; `~/.bound/...` on Linux).
  Nothing about the live testnet changed: its chain, addresses and data stay as they were.
- **Mainnet** has the project owner's economics, Bitcoin's exact schedule: 10-minute blocks, 50
  BOUND halving every 210,000 blocks, 20,999,999.9769 BOUND in all, no premine.
  - Addresses start with **B**.
  - Its genesis is **provisional** (`launched = false`). A node on it logs a warning that its
    coins will be thrown away, and the wallet app labels it *MAINNET PREVIEW* in red.
  - Launch day mines the real genesis (milestone 15).
- **Regtest** is Bitcoin's private test network:
  - any CPU finds a block instantly, and the difficulty never moves;
  - halvings come every 150 blocks, so tests can reach them;
  - it's for trying things, and for app development, on one machine.

## Files

| File | What it does | Bitcoin equivalent | Tests |
|---|---|---|---|
| `core/params.*` | `mainnet_params`, `regtest_params`, `params_for(Network)`, `network_flag`; `AddressVersion` (1 or 2 bytes); `pow_no_retargeting`, `launched` | `kernel/chainparams.cpp`, `chainparamsbase.cpp` | `Networks.*` |
| `core/address.*` | addresses with a 1- or 2-byte version | `key_io.cpp` (`base58Prefixes`) | `Networks.MainnetAddressesStartWithB` |
| `core/pow.cpp` | regtest keeps every block's difficulty | `fPowNoRetargeting` | `Networks.RegtestNeverRetargets` |
| `app/datadir.h` | a data directory per network; `take_network` reads the flag | `common/args.cpp` (`GetDataDir`, `-chain`) | `Networks.ChosenOnTheCommandLine` |
| `node/node.*`, `app/*`, `qt/*` | every program on the chosen network | | all |

## Choices

- **The project owner chose** Bitcoin's schedule, 'B' addresses, and testnet as the default until
  launch.
- **Address prefix:** a 1-byte version can only fix a digit (2-9) as the first character of a
  37-byte payload. Two bytes give 1,730 versions (0x439a to 0x4a5b) that fix 'B' at exactly 52
  characters for every key. Mainnet uses 0x46fb, from the middle of that range. A 1-byte version
  equal to its first byte doesn't decode as mainnet, nor the reverse.
- **Ports:** P2P and RPC ports are 7333/7332 for mainnet and 18544/18543 for regtest. Testnet's
  18333/18332 are unchanged; 18333 is also Bitcoin testnet3's, which the project owner accepted.
- **Magic bytes:** every byte has its high bit set, as testnet's do, and differs from Bitcoin's.
- **Mainnet's difficulty is still Bitcoin's rule** (2,016 blocks). ASERT replaces it in milestone
  11, before launch, so nothing depends on it. Its initial difficulty is the easiest allowed for
  now. Launch day sets one that fits the hash rate expected then.
- **Regtest shares testnet's address prefix,** as Bitcoin's regtest does.

## Testing

- **`test_networks.cpp`:** every genesis is valid, and the networks differ in magic bytes, ports
  and genesis. Mainnet's supply comes to Bitcoin's 20,999,999.9769 exactly. Mainnet addresses
  start with 'B' at 52 characters for the lowest key, the highest and five others, and never decode
  on the testnet (nor the reverse). Regtest never retargets. The command-line flags parse as
  intended.
- **By hand:** a regtest node mined 36 blocks in 4 seconds on its own ports. A mainnet preview node
  gave a 52-character `B…` address, paid 50 BOUND a block, listened on 7333 and logged its warning.
- **Totals:** 393 tests pass on Windows (MSVC) and Linux (GCC 13).
