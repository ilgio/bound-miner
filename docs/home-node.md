# Running a node from home

Bitcoin has no central server. It runs on tens of thousands of nodes, many of them ordinary home
computers, and BOUND works the same way. This page covers making a home node one that others can
connect *to*, which is what lets the network grow without anyone renting a server.

## How nodes find each other

- **Every node dials out.** It needs no setup for that: on start-up it connects to addresses it
  already knows (`peers.dat`), to `--connect` peers, or to what the seeds hand out.
- **Some nodes also take connections in.** Only these can be found by newcomers. A node behind a
  home router can't, unless the router forwards the P2P port (18333) to it.
- **A node that takes connections says so.** Right after connecting to a peer, and then about once
  a day, it sends that peer its own address. The peer passes it on, and so on across the network.
  That's Core's `AdvertiseLocal`. Each peer you dial tells you where it sees you coming from, and
  the node advertises the address most of them agree on, with the port it listens on. Only peers
  you dialled count, because anyone can connect in and claim anything.
- **The address book keeps it fresh.** An address that hasn't been vouched for in 30 days is
  forgotten. So is one that never answered after 3 tries.

`bound-cli getnetworkinfo` shows what the node knows about itself:

    "connections_in": 3,          // anyone connected in: you're reachable
    "connections_out": 8,
    "localaddresses": [           // the first is the one advertised
      { "address": "203.0.113.7", "port": 18333, "source": "peers", "peers": 8 }
    ]

The node also logs `advertising our address ...` whenever that address changes.

## Making your node reachable

1. **Check you have a public IP of your own.** Many ISPs share one public address among many
   customers (carrier-grade NAT, CGNAT). Then no port forwarding can work. Open your router's status
   page and find its WAN or internet IP:
   - the same as what a "what is my IP" site shows: you're fine;
   - different, or in 100.64.x.x to 100.127.x.x, or in 10.x: you're behind CGNAT. See
     [Behind CGNAT](#behind-cgnat).
2. **Give your PC a fixed LAN address.** In the router, use DHCP reservation (or "static lease") for
   your PC's IP, so the forward below keeps pointing at it.
3. **Forward TCP port 18333** in the router (often under *NAT*, *Port forwarding* or *Virtual
   server*) to that LAN IP, port 18333. If your router has UPnP, NAT-PMP or PCP, the node doesn't
   use them; forward by hand.
4. **Let it through the firewall.** Windows asks the first time `boundd.exe` listens: allow it on
   *private* networks. On Linux with ufw: `sudo ufw allow 18333/tcp`.
5. **Run the node.** It needs no special option:

       boundd node

   If the router forwards a different outside port (say 28333 to your 18333), or your outside
   address isn't the one peers see, tell the node with `--externalip 203.0.113.7:28333`.
6. **Check it from outside.** Ask someone elsewhere to run `boundd node --connect <your IP>`, or use
   any online "open port checker" on port 18333 while the node runs. Within a day of others
   learning your address, `connections_in` should rise above 0.

A home IP usually changes now and then. The node notices on its own: its peers report the new
address, and it advertises that instead.

## Becoming a seed

Newcomers need one address to start from. Until the network has seed servers, yours can be it:

- **A stable name.** Get a free dynamic-DNS name (DuckDNS, No-IP, or one built into your router)
  that follows your home IP.
- **The built-in list.** Put it in `p.seeds` in [src/core/params.cpp](../src/core/params.cpp), for
  example `p.seeds = {"bound-yourname.duckdns.org"};`. Every node built from the code tries it
  whenever it has too few peers.
- **Until then**, people can pass it themselves with `--seednode <name>` or `--connect <name>`.

That's how Bitcoin started: one node on Satoshi's computer, and people who connected to it.
After a few others run reachable nodes, they gossip each other's addresses and the network no
longer depends on any one of them.

## Behind CGNAT

No port forwarding can get through carrier-grade NAT. The choices:

- **Ask the ISP for a public IPv4.** Many sell it as a "static IP" add-on, and some give one free on
  request.
- **IPv6.** If your ISP routes IPv6 to you, the node can be reached over it directly; the router's
  IPv6 firewall still has to allow TCP 18333 in. Check with `ping -6 2606:4700:4700::1111`: if that
  fails, IPv6 isn't working yet.
- **A small server** (about $5 a month) as the reachable node, with yours connecting to it. Nothing
  about the coin changes; it's just a computer that's always on with a public address.

Even unreachable, your node is a full participant: it checks every block, relays transactions and
can mine. It just can't be the one newcomers find first.
