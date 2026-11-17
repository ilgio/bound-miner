# Milestone 13: releases ✅

**What it adds:** downloads. Pushing a tag like `v0.10.0` makes GitHub Actions build BOUND for
Windows and Linux, test the exact programs it's about to ship, check that they run on their own,
attest where they came from, and draft a release with them. [docs/release-process.md](release-process.md)
is the checklist.

## The packages

- **Windows: `bound-<version>-win64.zip`.** One folder: `bound-qt.exe` and the four command-line
  programs, the DLLs they load (Qt, libsodium, zlib, libpng, PCRE2, double-conversion), Qt's
  platform plugin (`platforms\qwindows.dll`) and Visual C++'s runtime, which Microsoft lets
  programs carry. It runs on a PC with nothing installed: unzip, double-click `bound-qt.exe`.
  About 12 MB zipped.
- **Linux: `bound-<version>-x86_64-linux-gnu.tar.gz`.** `bin/` with `boundd`, `bound-cli`,
  `bound-explorer` and `bound-seeder`; the systemd units in `share/bound/systemd`. libsodium,
  LevelDB and the C++ runtime are linked in statically (vcpkg's `x64-linux`), so the programs need
  only glibc. That's built on Ubuntu 22.04, so they run on glibc 2.35 or newer: Ubuntu 22.04,
  Debian 12, Fedora 36 and later. There's no Linux wallet app in it yet. Qt can't be linked in that
  simply, and an AppImage is a job for later. Linux users build `bound-qt` from source, which takes
  minutes with the distribution's Qt.
- **Both** carry `README.txt` (what each program is, how to start, how to check the download),
  BOUND's `LICENSE`, and `licenses/` with the license of every library built in. On Windows that
  includes Qt's LGPL, with where to get the Qt source that was used.

## How a release is checked before anyone downloads it

1. The tag, `CMakeLists.txt`'s version and `docs/release-notes/v<version>.md` must agree.
2. **Every test** runs against the Release build being packed, not just CI's Debug build.
3. **Each package is installed and checked from where it landed:**
   - **On Windows** this runs with nothing but `System32` on PATH. Every program answers
     `--version` with the right version, and `boundd --regtest demo` mines, pays and reorganises.
     Then `bound-qt.exe` is started on regtest. Its window has to open, which needs every Qt DLL
     and the platform plugin. Its node has to answer `bound-cli` with the right chain and user
     agent. Stopping the node has to close the app. This can't be fooled by the build machine's
     own copy of Qt: Qt looks for its plugin only beside the program (checked with
     `QT_DEBUG_PLUGINS=1`). With `platforms\` taken out, the app can't start.
   - **On Linux,** `ldd` must show nothing outside glibc, and `objdump` must show no symbol
     newer than glibc 2.35. Every program answers `--version`, and `boundd --regtest demo` runs.
4. `SHA256SUMS` is written, and **every file is attested** with
   `actions/attest-build-provenance`: a Sigstore-signed statement, kept by GitHub, that this
   repository's release workflow built that exact file from that commit.
   `gh attestation verify <file> --repo Sonic-Bound/Bound` checks it.
5. The release is a **draft**: a person reads it, tries a package and publishes it.

## Also new

- **One version, one place.** `project(bound VERSION 0.10.0)` in `CMakeLists.txt` feeds
  `--version`, the user agents (`/boundd:0.10.0/`, `/bound-seeder:0.10.0/`), the wallet app's About
  box, the Windows file properties and the package names. It used to be written out in four places.
- **The git commit** is recorded at every build (`cmake/build_info.cmake`), with `-dirty` if files
  had changed, so `boundd --version` names the exact source:

      boundd 0.10.0 (commit 0123456789ab)
      Copyright (c) 2026 BOUND. MIT license.
      Source: https://github.com/Sonic-Bound/Bound

- **Windows file properties.** Every `.exe` now has the BOUND icon, a description and its version
  (Explorer, *Properties > Details*), from `cmake/program.rc.in`.
- **`cmake --install`** works on both systems, so a Linux machine can also put the programs in
  `/usr/local` from source.
- **CI on Windows builds the wallet app too,** so `bound-qt`'s tests run on Windows on every push.
  That also keeps Qt in the vcpkg cache that the release workflow restores. The cache is now saved
  as soon as vcpkg finishes, so a failing test doesn't throw an hour of Qt away.

## Why not windeployqt

The roadmap planned to use Qt's own deployment tool. Tried on vcpkg's Qt, it copies Qt's DLLs and
plugins but not the libraries those DLLs need (zlib, libpng, PCRE2, double-conversion), because
vcpkg builds them separately. It would also have added plugins the app doesn't use. CMake's
`install(RUNTIME_DEPENDENCY_SET)` reads each program's imports and finds every DLL they name in
vcpkg's `bin`, Qt's or not. The plugin is the one file named by hand. The Windows check above
proves the result is complete.

## Files

| File | What it does | Bitcoin Core equivalent |
|---|---|---|
| `.github/workflows/release.yml` | check, build both packages, attest, draft the release | Guix builds, `contrib/guix` |
| `contrib/release/package-windows.ps1`, `package-linux.sh` | build, test, install, check, pack | `contrib/guix/libexec/build.sh` |
| `contrib/release/README.txt` | the README inside each package | `README.md` in the tarball |
| `cmake/build_info.cmake`, `src/util/version.*` | the version and commit everywhere | `cmake/script/GenerateBuildInfo.cmake`, `clientversion.cpp` |
| `cmake/program.rc.in` | Windows icon and version resources | `src/*-res.rc` |
| `docs/release-process.md`, `docs/release-notes/` | how to release, what's in each | `doc/release-process.md`, `doc/release-notes/` |
