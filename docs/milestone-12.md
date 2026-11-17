# Milestone 12: hardening for real value ✅

**What it adds:** the checks a coin needs before its coins are worth stealing, run on every push,
and a miner fast enough for mainnet.

- **Sanitizers.** `-DBOUND_SANITIZE=address,undefined` (or `thread`) builds everything so that a
  memory error, undefined behaviour, a leak or a data race stops the tests on the spot. CI runs
  the whole suite under ASan + UBSan and, separately, under TSan.
- **Fuzzing.** `bound_fuzz` holds 11 targets, one for every place untrusted bytes come in:

  | Target | What it feeds |
  |---|---|
  | `peer` | a hostile peer: messages of every type with fuzzed payloads, a handshake, blocks made on the spot and then mangled, raw bytes, time passing. On regtest, so fuzzed blocks can be valid and reach deep into validation |
  | `transaction`, `block` | decoding, with a check that whatever decodes re-encodes to the same bytes (no malleable encodings) |
  | `p2p_payload` | every message payload decoder, with round trips |
  | `message_stream` | cutting a byte stream into messages, however it's chunked |
  | `text` | hex, Base58, Base58Check, Base64, all three networks' addresses (one spelling each), amounts, the console's word splitting |
  | `difficulty` | compact targets and ASERT with arbitrary inputs: always within [1, limit], never harder later |
  | `saved_state` | peers.dat and banlist.dat |
  | `dns` | the seeder's DNS parser, which strangers reach over UDP |
  | `http`, `json_rpc` | the HTTP server (the explorer is public) and the JSON-RPC layer |

  - **Two builds.** Built with clang's libFuzzer (`-DBOUND_FUZZ=libfuzzer`) the targets are
    coverage-guided, and CI runs each for 45 seconds with ASan + UBSan. Built plainly (any
    compiler, Windows too), `bound_fuzz` mutates each target's seed inputs itself, and ctest runs
    every target 300 times.
- **CI on Windows.** MSVC with vcpkg, alongside Linux, with the vcpkg packages cached between runs.
- **A faster miner.**
  - **Midstate:** the header's first 64 bytes are hashed once, and each nonce costs one SHA-256
    compression plus the second hash. That doubles one thread's speed, from ~650,000 to
    ~1,270,000 hashes a second on the development machine (i5-10400F).
  - **Threads:** `BackgroundMiner` runs any number of threads, each on its own range of coinbase
    extra nonces so no two ever hash the same header. By default it uses every core but one:
    ~4.5 million hashes a second with 11.
  - **Controls:** `--minethreads <n>`, the third parameter of `setmining`, `threads` in
    `getmininginfo`, and a thread count on the wallet app's Mining page.
- **[A consensus review](consensus-review.md)** of overflow, merkle-tree tricks, malleability,
  duplicate transactions, timewarp, time rules, difficulty arithmetic, resource limits and the
  wallet's encryption. No bug found. The one risk, Ed25519 verification rules, is now guarded by a
  minimum libsodium version and by tests.
- **[SECURITY.md](../SECURITY.md):** how to report a vulnerability privately.

## What the tools found

- **UBSan:** an empty key reached LevelDB with a null pointer, which LevelDB passed to `memcmp`
  (undefined even for zero bytes). Now `slice()` gives LevelDB its own empty slice.
- **MSVC's checked iterators,** in the plain fuzzer: the fuzzer's own input reader read 2 bytes
  past its input. It called `std::min(u16(), remaining())`, and function arguments may be
  evaluated in any order: MSVC evaluated `remaining()` first. Fixed by reading the length first.
- **Fuzzing:** about 11 million libFuzzer runs on the development machine (35 seconds a target,
  plus 7 minutes more for `peer`) found nothing more, and nor did TSan.

## Files

| File | What it does | Bitcoin Core equivalent |
|---|---|---|
| `fuzz/fuzz.h`, `fuzz/main.cpp` | target registry, input reader, the libFuzzer entry point and the plain mutating runner | `src/test/fuzz/fuzz.cpp`, `FuzzedDataProvider` |
| `fuzz/targets.cpp` | the decoder and parser targets | `src/test/fuzz/deserialize.cpp`, `http_request.cpp`, ... |
| `fuzz/target_peer.cpp` | the hostile-peer target | `src/test/fuzz/process_messages.cpp` |
| `core/miner.*` | midstate hashing, threads | `CBlockHeader` midstate in external miners; Core's old internal miner |
| `core/leveldb_store.cpp` | the null-pointer fix | |
| `.github/workflows/ci.yml` | Linux, ASan+UBSan, TSan, libFuzzer, Windows | `.github/workflows/ci.yml`, OSS-Fuzz |
| `docs/consensus-review.md`, `SECURITY.md` | the review, the reporting policy | `SECURITY.md` |

## Running it yourself

    # sanitizers (Linux, GCC or clang)
    cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBOUND_GUI=OFF -DBOUND_SANITIZE=address,undefined
    cmake --build build/asan && ctest --test-dir build/asan
    # coverage-guided fuzzing (clang)
    CC=clang CXX=clang++ cmake -S . -B build/fuzz -G Ninja -DBOUND_GUI=OFF -DBOUND_FUZZ=libfuzzer -DBOUND_SANITIZE=address,undefined
    cmake --build build/fuzz --target bound_fuzz
    FUZZ=list build/fuzz/bound_fuzz                        # the targets
    FUZZ=peer build/fuzz/bound_fuzz -max_total_time=600    # one, for ten minutes
    # any compiler: mutate seeds, or replay a saved input
    build/msvc/Debug/bound_fuzz all --runs 10000
    build/msvc/Debug/bound_fuzz peer crash-peer-1234
