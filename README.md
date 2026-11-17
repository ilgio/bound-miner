# bound-miner

NVIDIA GPU (CUDA) miner for **BOUND** (algorithm BoundHash), by ilgio.
An unofficial build of BOUND 1.1.0 (https://github.com/Sonic-Bound/Bound), MIT license.
The node is built in: no pool, no separate node to run.

**Developer fee: 5% of each block mined on mainnet goes to the developer** ([DEVFEE.md](DEVFEE.md)).
The splash at startup shows it.

```
 _                               _                   _
| |__    ___   _   _  _ __    __| |       _ __ ___  (_) _ __    ___   _ __
| '_ \  / _ \ | | | || '_ \  / _` | ____ | '_ ` _ \ | || '_ \  / _ \ | '__|
| |_) || (_) || |_| || | | || (_| ||____|| | | | | || || | | ||  __/ | |
|_.__/  \___/  \__,_||_| |_| \__,_|      |_| |_| |_||_||_| |_| \___| |_|

  bound-miner 0.5   NVIDIA GPU (CUDA) miner for BOUND, algorithm BoundHash
  node       boundd 1.1.0
  author     ilgio
  dev fee    5% of each block mined, to BW2WEtNxrj1jBkydkG9qWdDGZkyfZjcdx61CEWfEsCrz8aheLdVz (DEVFEE.md)
  network    mainnet
  address    <your address>
  gpu        0: NVIDIA GeForce RTX 4090, 24564 MiB
  stats      ./bound-cli getmininginfo
```

## Download and run

Download `bound-miner-0.5.tar.gz` from [Releases](../../releases), then:

    tar xzf bound-miner-0.5.tar.gz
    cd bound-miner-0.5
    ./bound-miner --mine <public address> --gpu 0

| Option | |
|---|---|
| `--gpu 0,1,2` | the GPUs to mine on |
| `--minethreads <n>` | mine on the CPU too, with n threads |
| `--gpubatch <n>` | hashes in flight per GPU (default: chosen for the card) |
| `--connect <host[:port]>` | a node to sync from, when it finds no peers on its own |
| `--datadir <dir>` | data directory (default `~/.bound/mainnet`) |

- `./bound-miner` runs the node with mining on and starts it again if it stops with an error. Ctrl+C stops it.
- It mines only once the node is in step with the network, so no work is lost on a stale chain.
- `./bound-cli getmininginfo` shows hashrate, GPUs and blocks found.
- `./bound-gpubench --gpu 0 --check 20` measures a card and checks its results on the CPU.
- Every nonce a GPU finds is checked again on the CPU before a block is built.
- Only one node per data directory: a second node on the same machine needs `--datadir <another dir>`.
- The wallet never goes on a mining machine: mine to a public address.

## Speed

| GPU | H/s |
|---|---|
| RTX 4090 | about 365k |
| RTX 5070 Ti | about 298k |

## What is in this repository

- The miner: download it from [Releases](../../releases). It ships as binaries; its source is not published.
- [DEVFEE.md](DEVFEE.md), [CHANGELOG.md](CHANGELOG.md), [NOTICE](NOTICE): the fee, what changed, credits.
- Everything else (`src`, `tests`, `CMakeLists.txt`, ...) is **BOUND 1.1.0, unchanged**, from
  https://github.com/Sonic-Bound/Bound (MIT license, [LICENSE](LICENSE)); its own README is
  [BOUND-README.md](BOUND-README.md). Built from these files you get the official CPU-only client, not this miner.

## Requirements

Linux x86-64 with glibc 2.38 or newer (Ubuntu 24.04), an NVIDIA driver, an RTX 40 or 50 series card.
Everything else is in the package folder.

Mining and young software carry risk: use at your own risk.
