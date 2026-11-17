# Milestone 5: the chain on disk ✅

**What it adds:**
- `boundd` keeps its chain in a data directory and carries on where it stopped.
- A crash at any moment, even in the middle of a reorg, leaves a chain that opens cleanly. The tip
  and the UTXO set always agree.
- Damaged block files are caught at startup, and only one node can use a data directory at a time.

## Files

| File | What it does | Bitcoin Core equivalent | Tests |
|---|---|---|---|
| `core/storage.*` | `KeyValueStore` and `WriteBatch`, `BlockStore`; memory versions of both; `FlatFileBlockStore` | `dbwrapper.h`, `node/blockstorage.cpp`, `flatfile.cpp` | `MemoryStore.*`, `MemoryBlockStore.*`, `FlatFileBlockStore.*` |
| `core/leveldb_store.*` | `LevelDbStore`: the database on LevelDB, every write synced | `dbwrapper.cpp` | `LevelDbStore.*` |
| `core/chaindb.*` | the database layout: keys and record encodings | `txdb.cpp` | `ChainDb.*` |
| `core/validation.*` | `Chainstate` on storage: creates, loads, verifies, one write per change | `validation.cpp` (`LoadBlockIndex`, `FlushStateToDisk`, `VerifyDB`) | `PersistenceTest.*`, `CrashTest.*`, `DiskPersistence.*` |
| `app/main.cpp` | `--datadir`; `mine` on the stored chain; `info`, `balance` | `bitcoind`, `bitcoin-cli getblockchaininfo` | |
| `triplets/x64-windows.cmake` | builds the vcpkg packages with the developer shell's MSVC version | | |

`Chainstate(params)` still works entirely in memory, now on the memory stores. The other tests and
`boundd demo` run unchanged, through the same code the disk version uses.

## The data directory

    %APPDATA%\BOUND\testnet\          (or --datadir <dir>)
      blocks\blk00000.dat, ...         every block received, appended; a new file every 128 MiB
      chainstate\                      LevelDB: the block tree, the UTXO set, undo data, the tip

**Block files.** A block file is a run of records: the magic `BOND`, the block's length (u32),
then the block. An index entry points at a record by file number and offset.

**The database.** The first byte of each key says what it holds:

| Key | Value |
|---|---|
| `V` | format version, genesis hash, network name (opening another network's directory fails) |
| `T` | hash of the active tip; the stored UTXO set is the one at this block |
| `B` + block hash | the block's index entry: header, height, status, sequence, position in the block files |
| `C` + txid + output index | an unspent coin: amount, recipient, height, coinbase flag |
| `U` + block hash | undo data of a block on the active chain |

## Crash safety

- **Every change is one atomic, synced write.** That covers storing a header or block, connecting
  a block (its UTXO changes, its undo data and the new tip, all together), disconnecting one, and
  marking blocks invalid.
- **Block bytes go to disk before the index entry that points at them.** A crash in between leaves
  bytes at the end of a block file that nothing points at. They're never read, and the block is
  fetched again.
- **An interrupted reorg resumes.** A reorg is many writes. If it's cut short, the stored tip is
  somewhere along the way, with the matching UTXO set, and the next start finishes the job.
- **The crash test.** `CrashTest.AnyWriteCanBeTheLast` cuts the power after every possible number
  of writes during a reorg, in turn. Each time, the restarted node must end exactly where an
  uninterrupted one does. Writing the tip separately from the UTXO changes, or not resuming on
  startup, makes it fail.
- **Startup check.** The node re-reads the last 6 blocks and checks them against their index
  entries and undo data (`DiskPersistence.DamagedBlockFilesAreCaughtAtStartup`).
- **One node per data directory.** LevelDB locks it, so a second `boundd` on the same directory
  fails to open it.

## Where BOUND differs from Bitcoin Core

- **Undo data is in the database,** written in the same batch as the UTXO changes it reverses,
  instead of in separate `rev?????.dat` files. The two can't disagree, so no checksum is needed.
- **The whole UTXO set is also kept in memory,** where Core keeps a cache in front of the database.
  That's fine at testnet scale.
- **Every block is synced to disk as it's connected.** Core flushes its UTXO cache in batches for
  speed; doing it per block costs a few milliseconds each.
- **The order in which branches became complete is stored,** so a tie between two branches with
  equal work doesn't flip after a restart (`PersistenceTest.EqualWorkTieSurvivesARestart`).

## A build note

- vcpkg builds packages with the newest MSVC installed (14.44 on this machine). The developer
  shell, and so the project, uses Visual Studio's default (14.38).
- LevelDB only comes as a static library, and a static library from a newer toolset doesn't link
  into an older one's build.
- `triplets/x64-windows.cmake` builds the packages with the toolset the shell selected
  (`VCToolsVersion`), and `vcpkg-configuration.json` points vcpkg at it.

## Try it

    build\msvc\Release\boundd.exe mine 5 <address>
    build\msvc\Release\boundd.exe mine 3 <address>     # carries on from #5
    build\msvc\Release\boundd.exe info
    build\msvc\Release\boundd.exe balance <address>

Kill `boundd` in the middle of `mine` (Ctrl+C, or end it in Task Manager), then run `info`. It
opens at the last complete block.
