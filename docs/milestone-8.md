# Milestone 8: a public testnet ✅

**What it adds:** everything needed to run the testnet in the open.
- **Finding the network:** seed names built into the parameters, `--seednode`, and `bound-seeder`,
  a DNS seeder like Bitcoin's.
- **A block explorer:** `bound-explorer`, a web server that shows the chain, blocks, transactions,
  addresses and the mempool. It reads from a node that keeps indexes (`--index`).
- **Linux:** BOUND builds and passes every test on Ubuntu 24.04 with GCC 13. Units for running
  the node, the explorer and the seeder as systemd services are in `contrib/systemd`.
- **A node among strangers:** reviewing the network code for public use turned up a set of
  ways one peer could stall or swamp a node. They're closed (below).

## Files

| File | What it does | Bitcoin equivalent | Tests |
|---|---|---|---|
| `net/seeds.*` | looks up seed names on a thread of its own | `net.cpp` (`ThreadDNSAddressSeed`) | `Connman.FindsPeersThroughSeeds` |
| `seeder/crawler.*` | probes nodes, keeps the reachable ones | sipa's bitcoin-seeder | `Seeder.*` |
| `seeder/dns.*`, `seeder/dns_server.*` | DNS packets and the UDP server | bitcoin-seeder's `dns.cpp` | `Dns.*`, `Seeder.*` |
| `index/chainindex.*` | transaction, spent-output and address indexes | `-txindex`, plus what Esplora / Electrs keep | `ChainIndexTest.*` |
| `explorer/explorer.*` | the explorer's pages | (Esplora, mempool.space) | `Explorer*` |
| `rpc/client.*` | the calling side of JSON-RPC, for `bound-cli` and the explorer | `bitcoin-cli.cpp` | |
| `core/storage.*` | `scan`: walk keys in either direction, stopping early | LevelDB iterators | `MemoryStore.*`, `LevelDbStore.*` |
| `net/addrman.*`, `net/peerman.*`, `net/connman.*` | the fixes below | `addrman.cpp`, `net_processing.cpp`, `net.cpp` | `P2PTest.*`, `AddrMan.*` |
| `app/explorer_main.cpp`, `app/seeder_main.cpp` | `bound-explorer`, `bound-seeder` | | |
| `contrib/systemd/*.service` | the three servers as Linux services | `contrib/init/bitcoind.service` | checked with `systemd-analyze` |

## Choices (made by the project owner)

- **Build all of the seeding now,** not later: the seed mechanism, `--seednode`, and the DNS
  seeder. No names yet: the list in `params.cpp` is empty until there are machines to put in it.
- **A separate explorer program,** so web traffic never shares a thread or a crash with the node.
- **Keep the genesis block of 2026-11-01** (see "Launching", below).
- **Linux, tested under WSL** on this machine.

## A node among strangers

Each of these let a single peer (or a few) harm a node; each now has a test that fails without
the fix.

| Hole | Before | Now |
|---|---|---|
| **Withheld blocks** | Blocks were fetched only towards the one best header, and mining and transaction fetching paused while any better header was known. A peer announcing headers and never sending the blocks froze a node's sync, and every miner listening to it. | Each peer is asked for the blocks on the way to the best chain *it* has shown (Core's way). "Initial sync" needs all three: a better chain known, a tip over a day old, and a block within the last 2 minutes. So it ends on its own. |
| **Unrequested blocks** | Any block with valid proof of work was stored, and at minimum difficulty they're cheap. | Kept only if its chain reaches the anti-DoS threshold and it at least matches our tip, and not if it's more than 288 blocks ahead (Core's rules). |
| **Request floods** | A peer could send a megabyte of `getheaders` and the node queued every reply. | A peer's messages wait while 1 MB of replies to it is unsent, and its socket isn't read while 5 MB of its messages wait (Core's `fPauseSend` / `fPauseRecv`). TCP then holds the sender back. |
| **Address floods** | Every address a peer sent was taken, and when the book was full the oldest went, working or not. | A token bucket, one address per 10 seconds per peer plus a `getaddr` reply. Never-reached addresses are evicted before proven ones, and half of all dials go to proven ones. Hopeless addresses are forgotten (Core's `IsTerrible`). On a public node, private and local addresses are never kept, dialled or passed on (`--allowprivate` for a LAN). |
| **Self-connection nonce** | One nonce per node, and the claimed `addr_you` was trusted, so a peer could replay our nonce with any victim's address and we'd never dial the victim again. | A nonce per connection, as in Bitcoin. A replay only blacklists the address we dialled, the replayer's own. |
| **`notfound`** | Any hash a peer named was remembered as "it doesn't have that". | Only for blocks we asked that peer for. |
| **Inbound slots** | One host could take all 32. | 4 per IP (not counting this machine). |
| **The explorer's HTTP** | | 256 connections, 16 per IP, 10 seconds to send a request, 15 idle. |

## Seeds

A seed is a name that leads to nodes: a DNS seeder's name, or simply the name of an always-on
node. The testnet's seeds go in `params.cpp` (`p.seeds`), and `--seednode host[:port]` adds
more; `--noseeds` leaves the built-in ones out.

- **When they're asked:** at once if the node knows no addresses. Otherwise only if fewer than
  two outbound peers have turned up 11 seconds in, and again every 10 minutes while that lasts
  (Core's rule).
- **How:** the lookup runs on its own thread, so slow DNS never stalls the node.
- **What happens to the answers:** they go into the address book dated three days back (usable,
  but not passed on as fresh), and the node goes on from there.

## The DNS seeder

`bound-seeder` does what sipa's bitcoin-seeder does for Bitcoin. It crawls the network from one
or more `--node` addresses and answers DNS queries for its name with the addresses of nodes that
are up.

- **Crawling.** Up to 64 probes at a time.
  - Each probe: connect, `version` and `verack` both ways, `getaddr`, wait for the `addr` reply,
    hang up.
  - **A success:** the handshake completes with a full node of our protocol version. The
    addresses it sends back are probed in turn.
  - **Rechecks:** a good node is probed again every 30 minutes. A failing one backs off from 1
    minute up to a day.
  - **Forgetting:** an address that never answers in 5 tries is dropped, and so is one silent for
    two weeks after 10 tries.
  - **Saved:** everything goes to `seeder.dat`.
- **Handed out:** a node counts as good when all of these hold:
  - it's on the testnet's port (an A record can't carry a port);
  - its last probe answered;
  - it answers at least half the time;
  - its height is within a day of the median height of the nodes that answered recently. That's
    the median, not the maximum: one node claiming a huge height can't exclude the rest.
- **DNS.** An authoritative server for one name, over UDP.
  - **The name itself:** A and AAAA records with a fresh random sample in each reply, plus NS and
    SOA.
  - **Other names:** a name under the zone gets "no such name"; anything else is refused.
  - **Reply size:** 512 bytes (1,232 for clients that say they take more), never truncated.
  - **Abuse:** any UDP service can be used to bounce traffic at a forged address, so each address
    may ask 20 times in a burst and 10 times a second after.

To run one you need a domain. Delegate a name to the seeder's machine, in the parent zone:

    seed.example.org.  NS  ns.example.org.
    ns.example.org.    A   203.0.113.10

Then run `bound-seeder --host seed.example.org --ns ns.example.org --dnsaddr 203.0.113.10
--node <a node>`. `contrib/systemd/bound-seeder@.service` does this as a service (per network since milestone 14), with port 53 the
only privilege it has.

## The explorer

`bound-explorer` asks a node over JSON-RPC (with the cookie from its data directory) and serves
the pages. With `--public` it serves on every interface; without, only on 127.0.0.1, for a
TLS-terminating proxy in front.

| Page | Shows |
|---|---|
| `/` | height, difficulty, network hash rate, coins mined, the mempool, the latest blocks |
| `/block/<height or hash>` | a block and its transactions, with fees |
| `/tx/<txid>` | where it is, what each input spent, its fee, which outputs are spent and by what |
| `/address/<address>` | balance, totals, and its history 25 at a time |
| `/mempool` | what's waiting, with fees and fee rates |
| `/search?q=` | a height, hash, txid or address: redirects to its page |

**Safety.**
- Everything from the node or the URL is HTML-escaped. A transaction memo is whatever its sender
  typed, and a test feeds it `<script>` tags.
- Pages carry a Content-Security-Policy that allows no scripts at all.
- Only URLs of the right shape reach the node.
- Each page is built at most once every 2 seconds, however many ask for it.

**The node's side** (`--index`):
- **Indexes:** transaction locations, what spent each output, and per address its transactions,
  unspent outputs and totals. They live in `<datadir>/index`, one atomic write per block after the
  chain's own.
- **Reorgs:** a reorganisation is undone exactly. The chainstate now keeps the undo data of every
  block it ever connected, as Bitcoin's rev files do. A test checks that an index kept block by
  block through reorgs and restarts is byte for byte one built from scratch.
- **Gaps:** an index opened after the node ran without it catches up, backing out of blocks that
  left the active chain.

New RPC:
- `getrawtransaction <txid> 2`: inputs with what they spent, fee, which outputs are spent;
- `gettxout`;
- `getaddressbalance`, `getaddressdeltas`, `getaddressutxos`;
- `getindexinfo`;
- `getnetworkhashps`;
- `getblock <hash> 2` with each transaction's fee.

## Linux

    sudo apt install build-essential cmake ninja-build pkg-config \
                     libsodium-dev libleveldb-dev libsnappy-dev nlohmann-json3-dev libgtest-dev
    cmake --preset linux
    cmake --build --preset linux-release
    build/linux/Release/bound_tests

The system's packages stand in for vcpkg; libsodium comes through pkg-config.

**Checked on Ubuntu 24.04 (GCC 13) under WSL:**
- **Build and tests:** no warnings, and every test passes in Debug and Release, including those
  over real sockets. The POSIX socket code had never been compiled before.
- **Real processes:**
  - a node mined and indexed a payment;
  - the explorer served every page;
  - the seeder crawled the node and answered `dig`;
  - a second node, given only `--seednode`, found the first and synced;
  - all four stopped cleanly on SIGTERM.
- **As systemd services:** the three units ran under systemd with their sandboxing (no
  capabilities but the seeder's port 53, read-only system, a system-call allowlist).
  `systemd-analyze security` rates them 1.3 to 1.4, "OK".

## Launching the public testnet

Progress (2026-11-15): two always-on nodes run, on Google Cloud and Microsoft Azure, with the
DNS seeders `seed.bound.sarl` and `seed2.bound.sarl` and the explorer at
https://explorer.bound.sarl, all set up with `bound-server` ([milestone 14](milestone-14.md)).
Mainnet's `p.seeds` lists the seeders and both nodes; testnet's lists the nodes, since the seeders
move to mainnet on launch day ([milestone 15](milestone-15.md)).

1. **Machines.** At least two always-on servers, each running `boundd` (unit in
   `contrib/systemd`), with TCP 18333 open. Two are up.
2. **Seeds.**
   - Run `bound-seeder` on one or more of them, each with its DNS delegation.
   - Put the seed names in `params.cpp` (`p.seeds`), and also the nodes' own host names, which
     still work if a seeder is down.
   - Rebuild and publish.
3. **Mining.** Someone has to mine, or the chain doesn't move. `boundd node --mine <address>`.
4. **The genesis is kept (2026-11-01).** So:
   - Anyone can already be mining a private chain on it, and whichever chain has the most work
     when the network meets becomes the public one. Start mining on the public nodes early.
   - The first 1,440-block window counts from the genesis time, so the first difficulty period
     stays at the minimum. After that, the 4× limit per period lets the difficulty climb to the
     real hash rate within a few periods. At the minimum those periods pass in minutes.
5. **Minimum chain work.**
   - Once the public chain has some history, set `p.minimum_chain_work` to a bit less than its
     `chainwork` (`bound-cli getblockchaininfo`) in each release, so a node syncing from scratch
     can't be fed a cheap fake chain.
   - It's 0 now: there's no public chain yet.
6. **The explorer.** Run `bound-explorer` behind a TLS proxy (the unit's comments show Caddy),
   from a node with `--index`.

## Where BOUND differs from Bitcoin Core

- **No eclipse defences beyond the above.** No AddrMan buckets keyed by network group, no
  anchor connections across restarts, no protected outbound slots, no feeler connections.
- **The seeder speaks UDP only,** not TCP DNS. Its replies stay small enough not to need it.
- **The indexes are always all three together,** and they live in the node rather than in a
  separate indexer (Electrs, Esplora).
- **The explorer is server-rendered HTML with no JavaScript,** so there are no live updates;
  pages are at most 2 seconds old.

## Try it

    # a node that keeps the indexes, and an explorer for it: http://127.0.0.1:8080
    build\msvc\Release\boundd.exe --datadir $env:TEMP\bound-x node --local --index
    build\msvc\Release\bound-explorer.exe --datadir $env:TEMP\bound-x

    # a seeder on this machine, on 127.0.0.1:53, then ask it
    build\msvc\Release\bound-seeder.exe --host seed.bound.test --ns ns.bound.test --dnsaddr 127.0.0.1 `
        --node 127.0.0.1 --allowprivate --datadir $env:TEMP\bound-seeder
    nslookup seed.bound.test 127.0.0.1

    # another node that finds the first through a seed
    build\msvc\Release\boundd.exe --datadir $env:TEMP\bound-y node --local --nolisten --rpcport 18342 `
        --seednode 127.0.0.1:18333

**Notes:**
- **The default port.** The seeder only hands out nodes on port 18333, so for it to answer, the
  first node must use the default port, as above.
- **Port 53 on Windows.** Any program may use port 53 on Windows, and Windows `nslookup` asks only
  there: it ignores `-port`, as testing here showed.
- **On Linux,** port 53 needs root, so add `--dnsport 15353` and ask with `dig @127.0.0.1 -p 15353
  seed.bound.test`. The other commands work the same without `.exe`.
- **Not 5353,** which both systems keep for mDNS.
