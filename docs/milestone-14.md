# Milestone 14: launch infrastructure

**What it adds:** the tools to run the network's always-on machines. With them, each server is a
few commands. [Running a server](running-a-server.md) is the guide.

**The tools are done. The servers themselves are the project owner's to set up:**
- three nodes with three providers;
- a domain, with its DNS records;
- the explorer's address;
- a monitoring account.

Then the seed names go into `params.cpp`.

## bound-server

One script, in the Linux release packages (`share/bound/bound-server`). `install` links it into
`/usr/local/sbin`.

| Command | What it does |
|---|---|
| `install [<version> \| <package>]` | Downloads the newest release (or the version named) for this machine's architecture and checks it against the release's `SHA256SUMS`. It unpacks into `/opt/bound/<version>`, links the programs into `/usr/local/bin`, installs the systemd units, creates the `bound` user and restarts what runs. It keeps three versions, to go back with. |
| `node <network> [--index]` | `boundd@<network>`, started now and at every boot; opens its port |
| `explorer <network> <domain>` | turns on the node's indexes and starts `bound-explorer@<network>`. Installs Caddy, which fetches and renews the HTTPS certificate. Opens 80 and 443, and says whether the domain points here yet. |
| `seeder <network> <seed> <ns> [<email>]` | `bound-seeder@<network>`, crawling from the node beside it and bound to the machine's own network address. Opens UDP 53, prints the two DNS records to add. |
| `monitor <network> <url>` | `bound-health@<network>.timer`: every 5 minutes it pings the URL, or sends the reasons to `<url>/fail` (Healthchecks.io, Better Stack) |
| `status` | for each network: height, last block, peers, explorer and seeder; then every unit's state |
| `health <network> [--report]` | one check; the timer runs this |

**Firewalls** come in two layers:
- **The machine's own:** bound-server opens it, with ufw if it's on, or with the iptables rules
  that Oracle's images ship (everything but SSH rejected).
- **The provider's:** it can't reach that one, so it says which page of the console to visit.

**Moving an older server across:** a server set up before this milestone ran `boundd.service`.
`install` stops and removes it, carries `--index` over, and starts `boundd@testnet` on the same
data directory. The node in Google Cloud moves over that way without syncing again.

## Per-network services

`boundd@.service`, `bound-explorer@.service`, `bound-seeder@.service` and
`bound-health@.service`/`.timer` are templates: the instance is the network. A server runs
`boundd@testnet` now and adds `boundd@mainnet` on launch day, each with its own data directory
(`/var/lib/bound/<network>`) and ports. Options live in `/etc/bound/<service>-<network>.conf`
rather than in the unit files, so upgrades can replace the units freely. They replace the three
testnet-only units of milestone 8.

## The explorer's /health

`GET /health` answers JSON for uptime monitors:

    {"height":1234,"network":"testnet","peers":5,"status":"ok","tip_age":140}

It's a **503** with a `problem` when:
- the node doesn't answer;
- it has no peers;
- the newest block is older than `--staleafter`. That defaults to twelve blocks' time and at least
  an hour: an hour on testnet, two on mainnet. Twelve blocks' time passes with no block by chance
  about once in 160,000 blocks, so a stale alert almost always means something is wrong.

Like every page it's built at most once every 2 seconds. `Cache-Control: no-store` keeps proxies
from serving an old answer.

## The seeder starts from its own node

`bound-seeder --node 127.0.0.1` now works. The crawler used to drop private and local addresses
even when the operator named them, so a seeder had to start from a public node elsewhere. Now
operator-given `--node` addresses are always probed. Only public addresses are ever handed out,
nor is a private address learned from gossip. A test checks both halves.

The old unit's comment said to bind the seeder to the server's public IP. On Google Cloud, Oracle
and AWS that address isn't on the machine (the provider forwards it), so the bind would fail.
bound-server binds the address on the machine's interface instead (`ip route get` shows it).

## ARM packages

The release workflow also builds `bound-<version>-aarch64-linux-gnu.tar.gz`, on GitHub's ARM
runners. It goes through the same tests and the same checks: nothing but glibc, here 2.39 or
older. Oracle's free ARM servers can run it, and a Raspberry Pi 4 or 5 makes a home node.
`package-linux.sh` picks the vcpkg triplet from the machine (`x64-linux`, `arm64-linux`).

## Fixed along the way

- **A node built from source couldn't move to a release package.** Found by trying the move in
  WSL, on a chain written by milestone 8's binaries. LevelDB compresses with Snappy when it was
  built with it, and Ubuntu's and Debian's LevelDB are. vcpkg's, which the packages use, wasn't, so
  it couldn't read those files: `Corruption: corrupted compressed block contents`, and the node
  restarted forever. The Google Cloud node was built from source like that. Now:
  - vcpkg's LevelDB has its `snappy` feature, so every release build reads compressed chains. A
    test writes one with Snappy, checks the tables really came out compressed, and reads it back.
  - The node writes uncompressed, as Bitcoin Core does, so any LevelDB at all can read what it
    writes. Hashes and keys barely compress anyway.
- **Linux release builds:** vcpkg builds libsodium with autotools there, so the release workflow
  now installs autoconf, automake and libtool. Found by building the package in WSL.
- **A GCC warning:** at `-O2`, GCC warned that a decoded address's key might be used before it was
  set. It's always filled; it's now zeroed first, which quiets the warning.

## Files

| File | What it does | Bitcoin Core equivalent |
|---|---|---|
| `contrib/deploy/bound-server` | the commands above | (none: Core leaves deployment to its users) |
| `contrib/systemd/*@.service`, `bound-health@.timer` | per-network services | `contrib/init/bitcoind.service` |
| `explorer/explorer.*` | `/health` | |
| `seeder/crawler.*`, `app/seeder_main.cpp` | `add_start`: the operator's starting points | sipa's seeder `-s` |
| `.github/workflows/release.yml` | the aarch64 package | Guix's `aarch64-linux-gnu` host |
| `docs/running-a-server.md` | providers, DNS records, monitoring, launch day | `doc/` |
