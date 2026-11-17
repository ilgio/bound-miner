# Milestone 6: the peer-to-peer network ✅

**What it adds:** `boundd node` joins the network over TCP.
- It syncs the chain from its peers, headers first.
- It relays new blocks and mines (optionally) while staying connected.
- It finds other nodes through address gossip.
- A peer that breaks a rule is cut off.

## Files

| File | What it does | Bitcoin Core equivalent | Tests |
|---|---|---|---|
| `net/socket.*` | `NetAddress`, name resolution, non-blocking `Socket`, `poll` over raw Winsock / BSD sockets | `netaddress.cpp`, `netbase.cpp`, `util/sock.cpp` | `Sockets.*` |
| `net/protocol.*` | message framing and every payload's encoding | `protocol.h`, `net.cpp` (`V1Transport`) | `Framing.*`, `Messages.*` |
| `net/addrman.*` | the known addresses: learn, pick, back off, `peers.dat` | `addrman.cpp` | `AddrMan.*` |
| `net/peerman.*` | the protocol logic, independent of sockets | `net_processing.cpp` (`PeerManager`) | `P2PTest.*` |
| `net/connman.*` | the poll loop: accepting, dialling, moving bytes | `net.cpp` (`CConnman`) | `Connman.*` |
| `core/miner.*` | `BackgroundMiner`: mines on its own thread | `node/miner.cpp` | via `boundd node --mine` |
| `core/validation.*`, `core/chain.*` | `best_header()`, `read_block()`, `block_locator()` | `validation.cpp`, `chain.cpp` | |

`PeerManager` never touches a socket. `P2PTest.*` wires whole networks of nodes together in memory
and passes bytes between them as TCP would; `Connman.TwoNodesSyncOverTcp` does the same over real
TCP on 127.0.0.1.

## Network choices (chosen by the project owner)

- **Raw OS sockets:** Winsock on Windows, BSD sockets elsewhere. Non-blocking sockets and one poll
  loop on one thread, as in Bitcoin Core. (The POSIX path is written but only Windows is tested.)
- **Port 18333.** That's also Bitcoin testnet3's port, so a Bitcoin testnet node can't run on the
  same machine with default settings; `--port` picks another.
- **Discovery:** `--connect` peers, plus address gossip (`getaddr` / `addr`). DNS seeds can come in
  milestone 8.

The network magic is `b0 d4 e7 f5`, picked like Bitcoin's: every byte has its high bit set, so it's
never ASCII text or valid UTF-8.

## The protocol

Every message is framed as in Bitcoin:

    magic (4) | command (12, ASCII, NUL-padded) | payload length u32 | first 4 bytes of sha256d(payload)

`Framing.MatchesBitcoinsVerack` checks this against Bitcoin's own `verack` bytes. Payloads are at
most 4 MB.

| Message | Payload | Purpose |
|---|---|---|
| `version` / `verack` | version, services, time, the receiver's address, our listening port, nonce, user agent, height | the handshake; nothing else is accepted before it |
| `ping` / `pong` | nonce | every 2 minutes; no pong within a minute drops the peer |
| `getheaders` / `headers` | block locator / up to 2,000 headers | header sync, and announcing new tips |
| `getdata` / `block` / `notfound` | block hashes / a block | downloading blocks |
| `getaddr` / `addr` | up to 1,000 addresses | learning about other nodes |

## How a node syncs

1. **Headers first.** After the handshake each side sends `getheaders` with a locator of its best
   header. The reply continues the chain from where the two agree. The receiver keeps asking until
   an empty reply, so batch size doesn't matter.
2. **Then blocks.** The blocks missing on the way to the best header are requested in order, at
   most 16 per peer at a time, and only from peers known to have them. A block that takes over 60
   seconds drops the peer, which frees its requests for others.
3. **New tips are announced as `headers`** (up to the last 8). A peer that can't attach them to its
   tree asks for the way there with `getheaders`.
4. **No mining while behind.** `--mine` pauses whenever the node knows of headers with more work
   than its tip, so it doesn't waste work on a stale chain.

## Address gossip

- **Learning addresses.**
  - Each `version` carries the sender's listening port. A node adds an inbound peer's IP with that
    port, and remembers outbound peers that answered.
  - Outbound connections ask `getaddr`, answered once per connection with up to 1,000 addresses.
- **Passing them on.** An address that's new to us, and fresh (seen within 10 minutes), goes on to
  2 random peers. Relaying only new addresses stops them circling.
- **Self-connections.** The `version` nonce is random per node. A node that receives its own nonce
  has dialled itself; it remembers that address and never dials it again.
- **Connections.** A node opens up to 8 outbound connections, counting `--connect` peers, which are
  always kept up. It accepts up to 32 inbound.
- **Retries.** A failed address waits 1 minute, then 2, 4, … up to a day.
- **Saved.** The known addresses go to `peers.dat` every 5 minutes and on exit.

## Where BOUND differs from Bitcoin Core

- **Blocks are announced with `headers`,** never `inv`. That's Bitcoin's BIP130 behaviour, always
  on. `headers` entries also omit Bitcoin's always-zero transaction count.
- **`version` carries the sender's listening port** instead of a full "addr_from".
- **A rule-breaking peer is simply disconnected.** There are no misbehaviour scores or ban lists
  yet, and no defence against floods of low-work headers (Core's minimum chain work and headers
  presync). Both came later, before the public testnet ([hardening notes](hardening.md)).
- **No AddrMan buckets,** the part of Core's design that resists eclipse attacks.
- **No transaction relay yet;** milestone 7 added it, with the mempool ([notes](milestone-7.md)).

## Try it

    # one node that mines (all interfaces: Windows may ask about the firewall)
    build\msvc\Release\boundd.exe node --mine <address>

    # three nodes on one machine, each with its own data directory and port
    build\msvc\Release\boundd.exe --datadir $env:TEMP\bound-a node --local --port 19401 --mine <address>
    build\msvc\Release\boundd.exe --datadir $env:TEMP\bound-b node --local --port 19402 --connect 127.0.0.1:19401
    build\msvc\Release\boundd.exe --datadir $env:TEMP\bound-c node --local --port 19403 --connect 127.0.0.1:19402

C finds A on its own: B passes A's address along. Ctrl+C stops a node, and it carries on from the
same chain the next time it starts. `info` can't read a data directory while its node is running,
because LevelDB locks it; since milestone 7, `bound-cli getblockchaininfo` asks the running node.
