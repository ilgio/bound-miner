# Hardening before a public testnet ✅

**What it adds:** a node can face peers it doesn't trust.
- **Discouragement and bans:** a peer that breaks a rule is turned away for a day, and an operator
  can ban addresses.
- **Headers presync:** a flood of cheap headers can't fill a node's memory and disk.

## Files

| File | What it does | Bitcoin Core equivalent | Tests |
|---|---|---|---|
| `net/banman.*` | discouraged and banned addresses, `banlist.dat` | `banman.cpp` | `BanMan.*` |
| `net/headerssync.*` | headers presync: check a low-work chain, then download it again to store it | `headerssync.cpp` | `HeadersSyncTest.*`, `HeadersSyncRetarget.*` |
| `net/peerman.*` | discourages rule-breakers; sends low-work headers through presync | `net_processing.cpp` | `P2PTest.*` |
| `net/connman.*` | doesn't dial banned or discouraged addresses, and doesn't let them in | `net.cpp` | `Connman.BannedAddressesCantConnect` |
| `core/pow.*` | `next_work_required` from plain values, for headers outside the tree | `pow.cpp` | `HeadersSyncRetarget.*` |
| `rpc/methods.*` | `setban`, `listbanned`, `clearbanned` | `rpc/net.cpp` | `NodeRpcTest.Bans` |

## Choices (made by the project owner)

- **Discourage for 24 hours, one strike** (Bitcoin Core since v22), plus manual bans through RPC
  that are saved to disk.
- **The anti-DoS work threshold plus headers presync** (Bitcoin Core v24), rather than checkpoints
  or a cap per peer.

## Discouragement and bans

- **Discouraged:** a peer that breaks a protocol rule is cut off as before, and its IP is now also
  discouraged for 24 hours. A discouraged address isn't dialled, isn't let in, and isn't passed on
  in `addr` replies.
  - Rule-breaking covers malformed messages, invalid blocks, headers or transactions, and the wrong
    network magic.
  - There's no score, as in Core since v22: nearly every offence is one an honest node never commits.
  - The list is kept in memory only, at most 50,000 addresses.
- **Never discouraged, only cut off:**
  - **Peers on this machine** (127.0.0.0/8, ::1). Every local node shares that address, so
    discouraging it would cut off all of them. That's Core's rule too.
  - **`--connect` peers,** because the operator chose them.
- **Not a strike:**
  - an old protocol version;
  - a stalled download or a missing pong;
  - a transaction with low fees or missing inputs;
  - a weak chain.
- **Bans:** `setban <ip> add [seconds]` bans an address (a day by default) and drops any connection
  from it. `setban <ip> remove`, `listbanned` and `clearbanned` manage the list, which is saved to
  `<datadir>/banlist.dat`. `--connect` peers are dialled even when banned.
- **By IP, not port:** both lists ignore the port, so a peer can't come back on another one.

## Headers presync

**The attack.** Headers are cheap to make on a testnet: at minimum difficulty a header costs about
65,000 hashes. A peer could send millions of valid headers forking off far back in the chain, and
before this change every one of them went into the block tree, in memory and in LevelDB.

**The threshold.** A chain's headers are stored straight away only if the chain has at least our
tip's work less 1,440 blocks' worth (a day), or `minimum_chain_work` from the params, whichever is
higher. A chain within a day of ours could overtake it soon, so it's worth keeping; one further
behind must show its work first. That's Core's `GetAntiDoSWorkThreshold`, with a day of BOUND
blocks in place of Core's 144.

**When it applies.** It's checked when a `headers` message brings new headers whose chain, counted
to the end of that message, falls short. One message that carries the work on its own is stored
straight away, as in Core.

**Presync.** A chain that falls short is downloaded twice, and nothing is stored until the first
pass has seen it reach the threshold.

1. **First pass.** Every header is checked as the block tree would check it: it must follow on from
   the one before, and have valid proof of work, the difficulty the rules require, and a time past
   the median and not too far ahead.
   - Without the tree, retargets are worked out from the last 1,441 timestamps presync remembers.
     `HeadersSyncRetarget` checks this against the tree's own rule.
   - The work is added up.
   - For one header in every 64, the node keeps a single commitment bit: a bit of a hash of the
     header's hash, keyed with a random salt only this node knows.
   - If the chain ends before reaching the threshold, it's dropped: it was never a threat. The peer
     isn't blamed.
2. **Second pass.** The node asks for the same headers again from the fork point.
   - They're checked again and matched against the commitment bits.
   - A header is stored only after 1,536 more headers have matched behind it (24 bits), or once the
     second pass reaches the threshold too.
   - A peer that sends a different chain the second time is caught, except with probability 2⁻²⁴
     per try. It isn't discouraged for it, because an honest peer may have reorganised in between.
3. **After that,** the rest of the chain syncs as usual, since it now has the work.

**Memory.** Presync keeps one bit per 64 headers, the last 1,441 timestamps and the 1,536-header
buffer. The number of bits is capped by time: timestamps must pass the median of the last 11, so a
chain can't gain more than about 6 headers a second since the fork.

**On a fresh node,** `minimum_chain_work` is what protects it. Raise it each release to the chain's
work at the time, as Core does. The testnet's was the public chain's work at height 7,000 in
milestone 15, and is 0 again since milestone 16 started the testnet over; mainnet's is 0 until it
has a chain. While it's 0 a fresh node stores any valid
headers, and only a synced node is protected, by the threshold.

## Where BOUND differs from Bitcoin Core

- **Presync validates fully.** It checks the exact difficulty, from the timestamps it remembers.
  Core only checks that each difficulty change is a permitted one.
- **The commitment settings are fixed** (1 bit per 64, 24-bit buffer) rather than tuned per release
  by a script.
- **Discouraged addresses are refused outright.** Core still lets them take a spare inbound slot,
  and evicts them first.
- **No subnet bans and no headers-sync timeout.** A peer that goes quiet in presync keeps its slot
  until the usual ping timeout.

## Try it

    bound-cli setban 203.0.113.7 add 3600
    bound-cli listbanned
    bound-cli setban 203.0.113.7 remove
    bound-cli getnetworkinfo        # headers_presyncs: peers in presync right now

A node with a long chain logs a short, separate chain that a peer offers like this:

    peer 127.0.0.1:51537: headers forking at #0 with too little work to store: presync
    peer 127.0.0.1:51537: presync: dropped a chain of 50 headers with too little work to store
