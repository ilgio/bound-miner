# Launch day

How BOUND's mainnet starts: the way Bitcoin did.

**Mainnet launched on 16 November 2026.** Its genesis is
`be44789ea517f65d9c690a4690e45ac5189777ff1d34b1ced3d73c3c20674bad`, and its message quotes a headline from that day's front page
of *The Indian Express*: *"BOUND 16/Nov/2026 12 NBFCs lose over Rs 65 crore in three months to cyber fraud in Delhi-NCR"*.

| Bitcoin, January 2009 | BOUND |
|---|---|
| The genesis block quotes that day's *Times*: "Chancellor on brink of second bailout for banks". Nobody could have mined the chain before that day. | The genesis quotes a headline from launch day, mined that day. |
| The genesis reward can never be spent. | The same: it pays a key nobody holds, and never enters the UTXO set. |
| No premine: Satoshi published the program, then mined like anyone could. | No premine. The release comes out, and block 1 is open to anyone who runs it. The project owner mines from the start, as Satoshi did. |
| Difficulty 1, the easiest the chain ever allows. | Difficulty 1 (`0x1e03ffff`): about 4.2 million hashes a block, a quarter of an hour for a PC on every core, as Bitcoin's difficulty 1 cost a PC. |
| Mined on ordinary CPUs: no machine was built for SHA-256d yet. | [BoundHash](boundhash.md), BOUND's own proof of work: no machine is built for it, and Bitcoin's can't compute it. |

Everything else is settled already ([the road to mainnet](mainnet-roadmap.md)): 10-minute blocks,
50 BOUND halving every 210,000 blocks, ASERT difficulty, addresses starting with `B`.

## Ready before the day

- [x] The code: mainnet's rules, its seeds (`seed.bound.sarl`, `seed2.bound.sarl` and the servers'
      addresses), `boundd genesis` to mine the genesis ([milestone 15](milestone-15.md)).
- [x] Two always-on servers (Google Cloud, Microsoft Azure), the domain `bound.sarl`, the
      explorer, two DNS seeders, monitoring ([milestone 14](milestone-14.md)).
- [x] BOUND's own proof of work, [BoundHash](boundhash.md) ([milestone 16](milestone-16.md)), and
      the testnet running on it (release 0.11.0 on both servers).
- [ ] A DNS record for testnet's explorer, which gives its name to mainnet's:
      `testnet-explorer` A `104.211.211.39` (DNS only).
- [ ] **TCP 7333**, mainnet's port, open in both providers' firewalls (Google Cloud: VPC network >
      Firewall; Azure: the VM's Networking > inbound port rule).
- [ ] A third server with another provider (optional for the day itself).

## The day

**1. The headline.** Pick one from the front page of a major newspaper that day. The message is
`BOUND <DD/Mon/YYYY> <headline>`, at most 100 characters of plain ASCII, for example
`BOUND 20/Nov/2026 <the headline>`.

**2. Mine the genesis.** On a PC, with every core (`boundd` from a build of the launch commit's
parent, or of `main`):

    boundd --mainnet genesis "BOUND 20/Nov/2026 <the headline>" now

It searches from the current second, about 4.2 million hashes on average: a quarter of an hour at
4,500 a second (a 6-core PC), sometimes two or three times that. It prints the lines for
`src/core/params.cpp`. Anyone can check them later: the same message and time always give the same
block, on any machine.

**3. The launch commit.** One commit, "Mainnet launch":
- `src/core/params.cpp`, `make_mainnet()`: the printed genesis lines, and `p.launched = true`.
- `src/core/params.h`: `kDefaultNetwork = Network::Mainnet`. Every program now joins mainnet
  unless told `--testnet`; their usage texts follow by themselves.
- `CMakeLists.txt`: `project(bound VERSION 1.0.0)`. From 1.0 a release is no longer marked as a
  pre-release.
- `tests/test_networks.cpp`: mainnet launched, mainnet the default.
- `README.md`: mainnet first, the testnet second; the explorers' new addresses.
- `docs/release-notes/v1.0.0.md`, written ahead: the genesis hash, time and message in place of its
  `LAUNCH-DAY` placeholders. The release workflow refuses notes that still have one.

All the tests pass, and `boundd --mainnet genesis "<message>" <time>` gives the same hash again.

**4. Release.** `git push`, then:

    git tag v1.0.0
    git push origin v1.0.0

The release workflow builds and signs the packages and drafts the release. Check it, then
**Publish release**.

**5. The servers.** On each, after the release is published:

    sudo bound-server install
    sudo bound-server node mainnet

On the explorer's server (Azure): testnet's explorer moves to its own name first, then mainnet's
takes the main one.

    sudo bound-server explorer testnet testnet-explorer.bound.sarl
    sudo bound-server explorer mainnet explorer.bound.sarl

The seeders move to mainnet (`seed` on Azure, `seed2` on Google Cloud):

    sudo bound-server seeder mainnet seed.bound.sarl ns1.bound.sarl      # Azure
    sudo bound-server seeder mainnet seed2.bound.sarl ns2.bound.sarl     # Google Cloud

And a new Healthchecks.io check for each server's mainnet node:

    sudo bound-server monitor mainnet https://hc-ping.com/<the new check>

**6. Block 1.** Start the 1.0.0 wallet app: it opens on mainnet now. On *Mining*, press *Start
mining*. At difficulty 1 one PC finds a block every quarter of an hour or so at 4,500 hashes a
second; ASERT raises the difficulty as others join. Newly mined coins can be
spent after 100 blocks, about a day at that pace.

**7. Tell people.** The GitHub release is the announcement: what BOUND is, how to download it,
the genesis message, and "no premine, CPU-mined from block 1". Anything else (a post on
Bitcointalk's *Announcements (Altcoins)* board, social media) points to it.

## After

- **Keep a node mining.** Difficulty never falls below 1, so if nobody mines, the chain waits, as
  Bitcoin's would.
- **Each release raises `minimum_chain_work`** to the chain's work at the time, so a new node can't
  be fed a cheap fake chain.
- **Bigger miners.** Until milestone 16 BOUND used Bitcoin's SHA-256d, and one of Bitcoin's mining
  machines (100 trillion hashes a second, millions of PCs) could have taken roughly 7,000 early
  blocks while ASERT caught up, then left the chain stalled for weeks. [BoundHash](boundhash.md)
  closed that: those machines can't compute it. Someone could still bring far more than a PC (many
  computers, or graphics cards once someone writes a miner for them). Then ASERT raises the difficulty
  to match, every block, and lowers it again once for every two days the chain falls behind after
  they leave. A chain is only as safe as the work behind it, so the more people mine, the safer it is.
