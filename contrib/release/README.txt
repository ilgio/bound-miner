BOUND
=====

A proof-of-work cryptocurrency written from scratch in C++20.
Source, documentation and releases: https://github.com/Sonic-Bound/Bound

These programs run BOUND's mainnet unless told otherwise; --testnet joins the testnet, whose
coins have no value. BOUND is young, experimental software: use it at your own risk.


The programs
  bound-qt        the desktop wallet: a full node with a window (in the Windows package)
  boundd          a node without a window: boundd node
  bound-cli       talks to a running node: bound-cli help
  bound-explorer  a block explorer for a node started with --index: http://127.0.0.1:8080
  bound-seeder    a DNS seeder, for those running the network's seeds

  Each answers --version. Each takes --mainnet (the default), --testnet or --regtest, and
  --datadir <dir>.


Getting started
  Windows: this folder runs as it is: double-click bound-qt.exe. To install BOUND instead, with a
  Start menu entry and an uninstaller, run bound-<version>-win64-setup.exe from the release page.
  The programs aren't code-signed yet, so Windows may say "Windows protected your PC". Check the
  download first (below), then choose More info, Run anyway.
  To mine, open Mining and press Start mining.

  Linux: bin/boundd node starts a node; bin/bound-cli getblockchaininfo shows how far it has synced.
  bin/bound-cli setmining true mines into the node's wallet (bound-cli createwallet makes one).
  The x86_64 programs need glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 or later); the
  aarch64 ones need 2.39 (Ubuntu 24.04, Debian 13, Raspberry Pi OS 13 or later).

  A server: sudo share/bound/bound-server install <this package>, then
  sudo bound-server node mainnet. It also sets up the explorer with HTTPS, a DNS seeder and
  health reports; bound-server with no arguments lists how.

  The chain and the wallet are kept in %APPDATA%\BOUND\mainnet on Windows and ~/.bound/mainnet on
  Linux (testnet in place of mainnet with --testnet). Write down the seed the wallet shows when it
  is made: it is the only way to get the coins back. For others to reach your node, allow TCP port
  7333 in the firewall (18333 for the testnet).


Checking the download
  SHA256SUMS, on the release page, lists the SHA-256 of every file:
    Windows:  Get-FileHash bound-<version>-win64.zip
    Linux:    sha256sum --ignore-missing -c SHA256SUMS

  GitHub also attests each file: proof that it was built by the project's release workflow, from
  the tagged source, and not changed since. With the GitHub CLI:
    gh attestation verify <file> --repo Sonic-Bound/Bound


Licenses
  BOUND is under the MIT license (LICENSE). The libraries built into it keep their own licenses,
  in licenses/: libsodium (ISC), LevelDB and Snappy (BSD 3-clause) and nlohmann/json (MIT). The
  Windows package also has Qt (LGPL v3), and zlib, libpng, PCRE2 and double-conversion, which Qt
  uses, and the wallet app has Project Nayuki's QR Code generator (MIT).
