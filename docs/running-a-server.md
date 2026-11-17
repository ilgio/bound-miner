# Running a BOUND server

A network needs machines that are always on: nodes that new nodes can find, DNS seeders that point
to them, an explorer people can check, and someone told when any of it breaks. For mainnet's
launch the plan is at least three nodes with three different providers, in different regions, so
that no one company's outage or policy can take the network down ([road to mainnet](mainnet-roadmap.md)).

`bound-server`, in the Linux release packages, does the work. Each job is one command.

## What a server needs

- **Hardware.** 1 CPU, 1 GB of memory, 10 GB of disk and a public IPv4 address. That runs a node,
  the explorer, a seeder and the web server together.
- **System.** Linux with systemd: Ubuntu 22.04 or 24.04, or Debian 12 or 13. On a 64-bit ARM
  server, Ubuntu 24.04 or Debian 13.
- **A fixed address.** Reserve or "promote" the server's public IP so it never changes. Seeds
  and DNS records point at it.

### Where to get one

Prices and free tiers change; these are as of late 2026.

| Provider | Cost | Notes |
|---|---|---|
| Google Cloud, e2-micro | free in us-west1, us-central1 and us-east1 | the first testnet node runs here |
| Oracle Cloud, Always Free | free: Ampere ARM, up to 4 cores and 24 GB in all; or two small AMD machines | use the `aarch64` package on ARM. Ports must be opened in the subnet's security list, and its images also block them in iptables (bound-server opens those) |
| Hetzner Cloud | a few euros a month | Germany, Finland, the US, Singapore |
| DigitalOcean, Vultr, Linode | about $5 a month | many regions |

Don't mine on cloud servers. Most providers' terms forbid it, and they close accounts for it.

## Setting one up

Create the server with SSH access, then on it:

    curl -fsSLO https://raw.githubusercontent.com/Sonic-Bound/Bound/main/contrib/deploy/bound-server
    sudo bash bound-server install           # the newest release, checked against its SHA256SUMS
    sudo bound-server node testnet           # the node, as the service boundd@testnet

Then open **TCP 18333** (testnet) in the provider's firewall, so other nodes can connect:
- **Google Cloud:** VPC network > Firewall.
- **Oracle:** the subnet's security list.
- **AWS:** the security group.

bound-server opens the machine's own firewall (ufw or iptables) by itself. Check it:

    sudo bound-server status

It shows each network's height, how long ago the last block came, peers, and what runs.

**A server that already runs `boundd.service`** (set up by hand before milestone 14): `install`
moves it to `boundd@testnet` on the same data directory, so nothing syncs again.

**Upgrading** is `sudo bound-server install` again. It restarts what runs, on the new version, and
keeps the last three versions: `sudo bound-server install 0.10.0` goes back. A release that replaces
a network's genesis (0.11.0 started the testnet over with BoundHash) makes the node move the old
chain aside by itself, to `old-chain-<its genesis>` in the data directory, and sync the new one; the
explorer follows. Going back to an older version after that starts its chain from scratch.

## The domain

The explorer's HTTPS address and the DNS seeders need a domain name. Any registrar sells one for
about $10 a year. Here it's `example.org`. In the domain's DNS settings add:

| Name | Type | Value | For |
|---|---|---|---|
| `explorer.example.org` | A | server 1's IP | the explorer |
| `seed.example.org` | NS | `ns1.example.org` | seeder 1: the seed name is handed to the seeder on server 1 |
| `ns1.example.org` | A | server 1's IP | |
| `seed2.example.org` | NS | `ns2.example.org` | seeder 2, on another server, so one going down doesn't matter |
| `ns2.example.org` | A | server 2's IP | |

Some registrars want only the part before the domain in the name field (`seed`, `ns1`).

## The explorer, over HTTPS

    sudo bound-server explorer testnet explorer.example.org

That turns on the node's indexes and starts `bound-explorer@testnet`. It also installs Caddy, a
web server that fetches a free certificate from Let's Encrypt and renews it by itself, and opens
ports 80 and 443. Open those in the provider's firewall too. A minute or two after the A record
points to the server, `https://explorer.example.org` works.

## A DNS seeder

    sudo bound-server seeder testnet seed.example.org ns1.example.org you@example.org

The seeder crawls the network, starting from the node beside it, and answers DNS for
`seed.example.org` with nodes that are up. It needs **UDP 53** open in the provider's firewall. It
prints the two records to add (the table above). A machine runs one seeder, for one network,
since only one program can hold its DNS port: setting one up for another network stops the first.
Check it from anywhere:

    dig seed.example.org A

Then put the seed names in `p.seeds` in [src/core/params.cpp](../src/core/params.cpp), so the
next release finds the network through them.

## Being told when something breaks

Two free services between them cover it:

- **Healthchecks.io** (or Better Stack heartbeats) for each server.
  1. Make a check with a 5-minute period and a 10-minute grace time.
  2. Copy its ping URL, then:

         sudo bound-server monitor testnet https://hc-ping.com/<the check's id>

  Every 5 minutes `bound-health@testnet` checks the node, and the explorer and seeder if they run
  there.
  - **Healthy:** it pings the URL.
  - **Not healthy:** it sends the reason to `<URL>/fail`.
  - **Server down:** the pings stop. Either way, you get an email.

  Unhealthy means:
  - the node doesn't answer;
  - it has no peers;
  - no block for twelve blocks' time, at least an hour (two hours on mainnet). On testnet that
    also means nobody is mining;
  - the explorer or the seeder doesn't answer.
- **An uptime monitor** (UptimeRobot, Better Stack) on `https://explorer.example.org/health`. It
  answers 200 while all's well and 503 with the reason otherwise, so this also catches an expired
  certificate or a DNS mistake.

## On launch day

[The launch](launch-day.md) publishes a release with mainnet's genesis. Then, on each server,
`sudo bound-server install` for that release, **TCP 7333** opened in the provider's firewall, and:

    sudo bound-server node mainnet
    sudo bound-server explorer testnet testnet-explorer.example.org   # testnet's explorer gets a name of its own
    sudo bound-server explorer mainnet explorer.example.org           # and mainnet's takes the main one
    sudo bound-server seeder mainnet seed.example.org ns1.example.org # the seeder moves to mainnet
    sudo bound-server monitor mainnet https://hc-ping.com/<another check>

(the explorer lines on the explorer's server, the seeder line on each seeder's). Testnet's node
and explorer keep running beside mainnet's: each network has its own data directory, ports and
services. Its seeders don't: each machine's DNS port answers for one network, so testnet nodes find
each other through the always-on nodes' addresses, which are in testnet's `p.seeds`.

## Keeping it safe

- **SSH:** log in with keys only, and turn on automatic security updates (`unattended-upgrades`).
- **RPC:** never open the RPC ports (18332, 7332). The node only listens on 127.0.0.1 for them,
  and that's where the explorer talks to it.
- **No funds on a public server.** A seed node needs no wallet.

## Where the pieces are

| File | What it does |
|---|---|
| [`contrib/deploy/bound-server`](../contrib/deploy/bound-server) | the commands above |
| [`contrib/systemd/boundd@.service`](../contrib/systemd/boundd@.service) | a node, one per network; options in `/etc/bound/boundd-<network>.conf` |
| [`contrib/systemd/bound-explorer@.service`](../contrib/systemd/bound-explorer@.service) | the explorer; `/etc/bound/explorer-<network>.conf` |
| [`contrib/systemd/bound-seeder@.service`](../contrib/systemd/bound-seeder@.service) | the DNS seeder; `/etc/bound/seeder-<network>.conf` |
| [`contrib/systemd/bound-health@.service`](../contrib/systemd/bound-health@.service), `.timer` | the health report every 5 minutes; `/etc/bound/health-<network>.conf` |

Files on the server:
- `/opt/bound/<version>`: the programs, with `/usr/local/bin` linking to the version in use.
- `/var/lib/bound/<network>`: each network's chain.
- `/etc/caddy/bound/`: the web server's sites.
