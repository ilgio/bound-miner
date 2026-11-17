#!/usr/bin/env python3
"""BoundHash, BOUND's proof-of-work hash: a reference implementation in plain Python.

It follows docs/boundhash.md step by step and is written for reading, not speed (a mainnet hash
takes about a second). The C++ in src/core/boundhash.cpp must give the same answers: the test
vectors in tests/test_boundhash.cpp come from this file.

    python3 boundhash.py <80-byte header in hex> [lines]
    python3 boundhash.py --vectors

prints the hash as block explorers show it (most significant byte first). `lines` is the
scratchpad's size in 128-byte lines, 4096 (512 KiB, mainnet's) unless given.
"""

import hashlib
import sys

MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1
MAINNET_LINES = 1 << 12


def sha256(data):
    return hashlib.sha256(data).digest()


def tagged_hash(tag, data):
    t = sha256(tag.encode())
    return sha256(t + t + data)


def rotr(x, n):
    return ((x >> n) | (x << (64 - n))) & MASK64


def blamka(x, y):
    return (x + y + 2 * (x & MASK32) * (y & MASK32)) & MASK64


def g(v, a, b, c, d):
    v[a] = blamka(v[a], v[b])
    v[d] = rotr(v[d] ^ v[a], 32)
    v[c] = blamka(v[c], v[d])
    v[b] = rotr(v[b] ^ v[c], 24)
    v[a] = blamka(v[a], v[b])
    v[d] = rotr(v[d] ^ v[a], 16)
    v[c] = blamka(v[c], v[d])
    v[b] = rotr(v[b] ^ v[c], 63)


def mix_round(v):
    g(v, 0, 4, 8, 12)
    g(v, 1, 5, 9, 13)
    g(v, 2, 6, 10, 14)
    g(v, 3, 7, 11, 15)
    g(v, 0, 5, 10, 15)
    g(v, 1, 6, 11, 12)
    g(v, 2, 7, 8, 13)
    g(v, 3, 4, 9, 14)


def fill_line(x, i):
    t = list(x)
    t[15] ^= i
    mix_round(t)
    return [t[k] ^ x[k] for k in range(16)]


def words(data):
    return [int.from_bytes(data[8 * k:8 * k + 8], "little") for k in range(len(data) // 8)]


def bound_hash(header, lines=MAINNET_LINES):
    """BoundHash of an 80-byte header: 32 bytes, least significant first (BOUND's uint256)."""
    assert len(header) == 80
    assert 2 <= lines <= 1 << 20 and lines & (lines - 1) == 0

    # 1. The seed and the starting state.
    seed = tagged_hash("BOUND/BoundHash", header)
    start = []
    for k in range(4):
        start += words(sha256(seed + bytes([k])))

    # 2. Fill.
    mem = [fill_line(start, 0)]
    mem.append(fill_line(mem[0], 1))
    for i in range(2, lines):
        prev = mem[i - 1]
        ref = ((prev[0] & MASK32) * (i - 1)) >> 32
        mem.append(fill_line([prev[k] ^ mem[ref][k] for k in range(16)], i))

    # 3. Mix.
    v = [start[k] ^ mem[lines - 1][k] for k in range(16)]
    for r in range(lines):
        line = mem[v[0] & (lines - 1)]
        for k in range(16):
            v[k] ^= line[k]
        product = v[0] * (v[1] | 1)
        v[2] = (v[2] + (product >> 64)) & MASK64
        v[3] ^= product & MASK64
        v[15] ^= r
        mix_round(v)
        for k in range(16):
            line[k] = (line[k] + v[k]) & MASK64

    # 4. The result.
    out = seed + b"".join(w.to_bytes(8, "little") for w in v)
    return tagged_hash("BOUND/BoundHash-result", out)


def display(digest):
    return digest[::-1].hex()


def vector_header(n):
    """The headers tests/test_boundhash.cpp hashes: version 1, prev = SHA-256("prev n"),
    merkle = SHA-256("merkle n"), time 1800000000 + n, bits 0x1e03ffff, nonce n."""
    return (
        (1).to_bytes(4, "little")
        + sha256(b"prev %d" % n)
        + sha256(b"merkle %d" % n)
        + (1800000000 + n).to_bytes(4, "little")
        + (0x1E03FFFF).to_bytes(4, "little")
        + n.to_bytes(4, "little")
    )


def main(argv):
    if argv[1:] == ["--vectors"]:
        for lines in (2, 64, MAINNET_LINES, 1 << 14):
            for n in range(3):
                print(lines, n, display(bound_hash(vector_header(n), lines)))
        return 0
    if len(argv) not in (2, 3):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    header = bytes.fromhex(argv[1])
    if len(header) != 80:
        print("a header is 80 bytes (160 hex digits)", file=sys.stderr)
        return 2
    lines = int(argv[2]) if len(argv) == 3 else MAINNET_LINES
    print(display(bound_hash(header, lines)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
