# Milestone 7: mempool, transaction relay, wallet, RPC ✅

**What it adds:** payments travel between nodes, and a running node can be asked things.
- **Mempool:** each node keeps the valid transactions waiting for a block, and a block template takes
  the best-paying ones.
- **Relay:** transactions spread across the network the way blocks do.
- **Wallet:** keys from one seed, encrypted with a passphrase. It follows the chain and the mempool.
- **RPC:** `boundd node` serves JSON-RPC on 127.0.0.1, and `bound-cli` talks to it.

## Files

| File | What it does | Bitcoin Core equivalent | Tests |
|---|---|---|---|
| `core/mempool.*` | the mempool: policy, replace-by-fee, limits, eviction, block selection, reorgs | `txmempool.cpp`, `validation.cpp` (`MemPoolAccept`), `node/miner.cpp` | `MempoolTest.*`, `FeeRate.*` |
| `core/validation.*` | `ChainListener`: told about every block connected or disconnected | `validationinterface.cpp` | via the mempool and wallet tests |
| `core/amount.*` | amounts as exact decimal text | `util/moneystr.cpp` | `Amounts.*` |
| `net/peerman.*` | `inv` / `getdata` / `tx` by wtxid, orphans | `net_processing.cpp`, `txrequest.cpp`, `txorphanage.cpp` | `TxRelayTest.*` |
| `net/connman.*` | lets the RPC server share the poll loop (`PollClient`) | | |
| `wallet/crypter.*` | the seed under the passphrase: Argon2id + XChaCha20-Poly1305 | `wallet/crypter.cpp` | `Crypter.*` |
| `wallet/wallet.*` | keys, history, balances, coin selection, payments, fee bumps | `wallet/wallet.cpp`, `wallet/spend.cpp`, `wallet/feebumper.cpp` | `HdWalletTest.*`, `WalletKeys.*` |
| `rpc/http.*` | a small HTTP/1.1 server and client on our own sockets | `httpserver.cpp` (libevent there) | `Http.*` |
| `rpc/server.*` | JSON-RPC 2.0 dispatch, cookie auth | `rpc/server.cpp`, `httprpc.cpp` | `JsonRpc.*` |
| `rpc/methods.*` | the methods | `rpc/*.cpp`, `wallet/rpc/*.cpp` | `NodeRpcTest.*` |
| `util/base64.*` | for HTTP Basic auth | `util/strencodings.cpp` | `Base64.*` |
| `app/cli.cpp` | `bound-cli` | `bitcoin-cli.cpp` | |

## Choices (made by the project owner)

- **Minimum relay fee: 1 unit per byte**, like Bitcoin's 1 sat/vB. A 1-input, 2-output payment
  is 191 bytes, so 191 units (0.00000191 BOUND).
- **Full replace-by-fee.** Any waiting transaction can be replaced by one that spends the same coins
  and pays more. That's Bitcoin Core's default since v28.
- **Wallet:** one random 32-byte seed derives every key, and the seed is stored encrypted under a
  passphrase.
- **RPC:** JSON-RPC over HTTP on 127.0.0.1 with cookie auth, as in bitcoind. JSON comes from
  nlohmann-json (vcpkg); the HTTP is written here on our own sockets.

## The mempool

**Accepting a transaction.** Checks run cheapest first, and signatures are checked last.

| Result | When |
|---|---|
| `Invalid` | it breaks a consensus rule: a coinbase, `check_transaction`, outputs over inputs, a bad signature |
| `NonStandard` | version isn't 1, over 100,000 bytes, or an output under the **dust limit of 420 units** |
| `MissingInputs` | it spends an output that's unknown or already spent |
| `Premature` | it spends an immature coinbase |
| `FeeTooLow` | it pays under 1 unit per byte |
| `Conflict` | it's a double spend that doesn't qualify as a replacement (below) |
| `TooLongChain` | more than 25 unconfirmed ancestors, or descendants for any ancestor (Bitcoin's limits) |
| `MempoolFull` | it pays too little to stay in a full mempool (50 MB of transactions) |

The dust limit works like Bitcoin's. Spending an output costs a 100-byte input on top of its own
40 bytes. At 3 units per byte (Bitcoin's dust relay fee) that's 420 units, so a smaller output
would cost over a third of its value to spend.

**Replacing a transaction.** A transaction may spend coins that a waiting one already spends. It
then replaces that one, together with everything built on it, if:
- its fee rate is higher than each transaction it directly conflicts with;
- its fee covers all the fees it evicts, plus 1 unit per byte of its own size;
- it evicts at most 100 transactions;
- it doesn't spend outputs of a transaction it evicts.

**Following the chain.**
- **A block connects:** its transactions leave the mempool, along with anything that double-spends
  them (and their descendants).
- **A reorganisation:** once the tip settles, the disconnected blocks' transactions and everything
  already waiting are checked again from scratch. Whatever the new chain contains or double-spends
  drops out. So does anything spending a coinbase that's immature again.
- **Expiry:** a transaction that waits two weeks is dropped, with its descendants.

**When it's full,** the transaction whose package (it plus its descendants) pays the lowest rate is
evicted, with its descendants (Bitcoin's descendant score).

**Building a block.** Block templates take packages (a transaction plus its waiting ancestors) by
fee rate, best first. That's Bitcoin's ancestor-feerate selection, so a child can pay for its
parent. `--mine` and `generatetoaddress` both use it. `estimatefee` gives the rate that would
make the next block if nothing else arrived: the minimum if everything fits, otherwise 1 unit per
byte above the lowest rate that made it.

## Relay

- **Announcing:** each accepted transaction goes out by wtxid (`inv`, type 5 as in Bitcoin's
  BIP339) to peers that don't have it. A copy with broken signatures has the same txid but a
  different wtxid, so it can't block the real one.
- **Fetching:** peers ask with `getdata` and get a `tx`. Each transaction is asked of one peer at a
  time, at most 100 per peer. If one doesn't arrive within 60 seconds, the next peer that
  announced it is asked.
- **New peers:** a new peer is told about the whole mempool. Nothing is fetched while blocks are
  still downloading.
- **Orphans:** a transaction whose parent hasn't arrived, or whose coinbase input matures in a
  block we don't have yet, waits as an orphan. There are at most 100, each kept 20 minutes. Orphans
  are retried when a parent arrives and whenever the tip moves.
- **Refused transactions:** a refused transaction isn't fetched again until the tip moves.
- **Bad peers:** only a transaction that can't be valid anywhere (`Invalid`) disconnects the peer.
  Low fees or missing inputs may just be a different view of the network.

## The wallet

**Keys.**

    key(branch, index) = KeyPair::from_seed(tagged_hash("BOUND/wallet-key", seed | branch | index))

The branch is 0 for addresses and 1 for change, and the integers are u32 little-endian. The seed
is stored only as `encrypt_secret(seed, passphrase)`. That's Argon2id (64 MiB, 2 passes) deriving
a key for XChaCha20-Poly1305, and the associated data binds it to the network. A wrong passphrase,
or a changed byte, fails the same way.

**Keypool.** The public keys of the next 100 unused keys on each branch are derived ahead and
stored in the clear. A locked wallet can still hand out addresses and recognise payments, and the
pool is topped up whenever it's unlocked. Unlocking keeps the seed in memory until the timeout or
`walletlock`.

**Transactions and coins.**
- The wallet stores every transaction that pays its keys or spends its coins, and where it was
  confirmed. It learns of them as a `ChainListener` and a `MempoolListener`.
- On startup it scans the blocks it missed, from where it last was. If that block left the active
  chain, it starts from the fork.
- Which coins are unspent isn't stored. It's worked out from the chain's UTXO set and the mempool,
  so it can't go stale.
- **Spendable:** confirmed and mature, or our own unconfirmed change. **Unconfirmed:** someone
  else's payment still in the mempool. **Immature:** mined, waiting out the 100 blocks.

**Paying.**
- Coins are chosen biggest first.
- Change goes to a new change key, unless it would be dust; then it goes to the miner.
- The output order is random, so it doesn't give away which output is change.
- `bumpfee` rebuilds one of our waiting transactions: the same inputs and payments, with the change
  cut (or more coins added) to beat the original and its descendants, as replace-by-fee requires.
- Our own transactions that end up in neither the chain nor the mempool are offered to the mempool
  again every 10 minutes, as long as their inputs are still unspent.

**Storage.** The wallet lives in `<datadir>/wallet`, its own LevelDB:

| Key | Holds |
|---|---|
| `M` | version, network, encrypted seed, next key indices |
| `K` + branch + index | a derived public key |
| `T` + txid | a wallet transaction |
| `B` | the last block seen |

## RPC

- **Where:** `boundd node` serves JSON-RPC 2.0 on 127.0.0.1, on port 18332, or `--rpcport`.
- **Auth:** at startup the node writes `__cookie__:<64 random hex digits>` to `<datadir>/.cookie`
  and deletes it on exit. Requests need it as HTTP Basic auth, compared in constant time.
- **Requests:** positional parameters. A batch (a JSON array) is fine.
- **Amounts** are decimal strings with 8 decimals (`"1.25000000"`), never floating point. They may
  be passed as strings or numbers. **Fee rates** are units per byte.
- **Errors** use Bitcoin Core's codes where one fits: -6 insufficient funds, -13 wallet locked,
  -14 wrong passphrase, -18 no wallet, -26 rejected.
- **Threads:** the RPC server is polled in Connman's loop, on the same thread as the P2P code, so
  nothing needs locks. A slow call (`generatetoaddress`, a wallet rescan) holds up networking
  until it's done.

| Area | Methods |
|---|---|
| chain | `getblockchaininfo` `getblockcount` `getbestblockhash` `getblockhash` `getblockheader` `getblock` |
| transactions | `getrawtransaction` (mempool or wallet; no index of the whole chain) `decoderawtransaction` `sendrawtransaction` |
| mempool | `getmempoolinfo` `getrawmempool` `getmempoolentry` `estimatefee` |
| network | `getnetworkinfo` `getconnectioncount` `getpeerinfo` |
| mining | `generatetoaddress` |
| wallet | `createwallet` `restorewallet` `getwalletinfo` `getbalance` `getnewaddress` `listunspent` `listtransactions` `gettransaction` `sendtoaddress` `bumpfee` `walletpassphrase` `walletlock` `walletpassphrasechange` `dumpseed` |
| other | `validateaddress` `help` `stop` |

`bound-cli [--datadir d] [--rpcport p] <method> [param]...` reads the cookie from the data
directory. Parameters are strings, except those a small table marks as numbers or true/false
(Bitcoin's `vRPCConvertParams`). `bound-cli help` lists the methods, and `bound-cli help <method>`
explains one.

## Where BOUND differs from Bitcoin Core

- **Orphans wait for their parents** rather than asking for them by txid.
- **Announcements go out at once,** without Core's random delays that hide where a transaction
  started.
- **No rolling minimum fee** after an eviction, and no fee estimation from history: `estimatefee`
  only looks at what's waiting now.
- **The mempool isn't saved** on shutdown. The wallet offers its own transactions again after a
  restart.
- **A reorganisation re-checks the whole mempool,** which is simple and fine at testnet sizes.
- **One wallet per node.** No address labels, and no watch-only keys.
- **A payment to an address beyond the keypool** is missed while the wallet is locked. That needs
  over 100 addresses handed out and unused while locked. `restorewallet` from the seed finds it.
- **Passphrases are passed on the command line,** so they can end up in shell history, as with
  `bitcoin-cli walletpassphrase`.

## Try it

    # a node, its wallet, and coins (generatetoaddress mines at once; fine at testnet difficulty)
    build\msvc\Release\boundd.exe --datadir $env:TEMP\bound-a node --local --port 19401
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-a createwallet "my passphrase"
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-a getnewaddress
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-a generatetoaddress 101 <that address>
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-a getbalance

    # a second node with its own wallet; pay it
    build\msvc\Release\boundd.exe --datadir $env:TEMP\bound-b node --local --port 19402 --rpcport 19602 --connect 127.0.0.1:19401
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-b --rpcport 19602 createwallet "another one"
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-b --rpcport 19602 getnewaddress
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-a walletpassphrase "my passphrase" 600
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-a sendtoaddress <b's address> 2.5
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-b --rpcport 19602 getwalletinfo   # unconfirmed 2.5
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-a generatetoaddress 1 <a's address>
    build\msvc\Release\bound-cli.exe --datadir $env:TEMP\bound-b --rpcport 19602 getbalance      # 2.50000000

`createwallet` prints the seed once. Write it down: `restorewallet <seed> <passphrase>` brings back
every key and its history on any node.
