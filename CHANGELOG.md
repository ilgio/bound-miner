# Changelog

## 0.5
- One folder: `./bound-miner`, no `bin/`; the libraries sit next to the programs.
- A splash at startup: version, node, author, developer fee, network, address, GPUs.
- The developer fee is checked on every block the miner works on or finds; a binary changed to
  skip it stops mining.

## 0.4
- A faster GPU kernel: four threads per hash, every memory access 32 contiguous bytes.
  RTX 5070 Ti 235k -> 298k H/s, RTX 4090 about 365k H/s.
- No mining until the node is in step with the network: with no peer, or while it catches up, the
  miner waits (`waiting for peers ...: not mining yet`). `--connect <host[:port]>` gives it a peer.
- `bound-gpubench`: a card's speed, with its results checked on the CPU.

## 0.2
- First release: CUDA mining in the BOUND node, `bound-miner` launcher that restarts on errors.
