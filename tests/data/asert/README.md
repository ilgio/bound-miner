# aserti3-2d test vectors

The official test vectors of Bitcoin Cash's ASERT difficulty algorithm (aserti3-2d, activated
2020-11-15), unchanged. They come from Bitcoin Cash Node's `qa-assets` repository,
`test_vectors/aserti3-2d/run01` … `run12`:
<https://gitlab.com/bitcoin-cash-node/bchn-sw/qa-assets/-/tree/master/test_vectors/aserti3-2d>,
under the MIT License (Copyright (c) 2018 the Bitcoin Cash Node developers).

Each file states an anchor (height, its parent's time, its nBits) and then lists, one per line,
`iteration height time target`: a parent block's height and time, and the nBits the next block
must have. They all use Bitcoin Cash's parameters: 600-second blocks, a half-life of 172,800
seconds, and a proof-of-work limit of `0x1d00ffff`. `tests/test_asert.cpp` checks
`asert_target` against every line.
