# Milestone 9: the desktop wallet ✅

**What it adds:** `bound-qt`, a desktop app that is a full node with a window, like Bitcoin-Qt.
- **One program.** Opening it starts a node inside it, and closing it stops that node cleanly. If
  a `boundd` is already running on the same data directory, the app uses that node instead and
  leaves it running.
- **Pages:**
  - **Overview:** balances and the latest transactions.
  - **Send:** checks the address, takes an optional memo and an automatic or custom fee, and asks
    to confirm.
  - **Receive:** your address.
  - **Transactions:** the history; double-click one for its details, or to raise the fee of a
    payment that hasn't confirmed.
  - **Mining:** start and stop.
  - **Network:** your peers, and whether others can reach you.
  - **Console:** RPC commands, and the node's log.
- **The wallet's life cycle:** create one (the seed is shown, and the dialog can't be dismissed
  until you tick that you wrote it down), restore one from its seed, back up the seed, change the
  passphrase, lock it. The wallet stays locked: spending asks for the passphrase and locks it again
  after.
- **Mining while the node runs:** the new `setmining` and `getmininginfo` RPCs. Until now, mining
  was either `--mine` at start-up or `generatetoaddress`, which blocks the node while it works.
- **The first seed:** `p.seeds` lists the project's always-on node, so a new node finds the
  network with no options.

## Files

| File | What it does | Bitcoin equivalent | Tests |
|---|---|---|---|
| `node/node.*` | the node (moved from `app/main.cpp`), so `boundd` and `bound-qt` run the same code | `node/` (`AppInitMain`) | everything that runs a node |
| `qt/nodehost.*` | runs the node on a thread of its own, or finds one already running | `qt/bitcoin.cpp` (`BitcoinCore`) | `WalletApp.*` |
| `qt/rpc.*` | JSON-RPC calls on a worker thread, answers back on the GUI thread | `qt/clientmodel`, `qt/walletmodel` | `WalletApp.*` |
| `qt/mainwindow.*` | the window: sidebar, status bar, menus, a refresh every 2 s | `qt/bitcoingui.cpp` | `WalletApp.RunsANodeInsideAndShowsItsWallet` |
| `qt/overview.cpp` … `qt/console.cpp` | the pages | `qt/overviewpage.cpp`, `sendcoinsdialog.cpp`, `rpcconsole.cpp`, … | ditto |
| `qt/dialogs.*` | passphrase, create/restore, seed, transaction details, About | `askpassphrasedialog.cpp`, `transactiondescdialog.cpp` | |
| `qt/style.*`, `qt/bound.qrc` | Fusion style in BOUND's colours (light or dark, as the system is); the logo and app icon compiled in, and the `.exe`'s icon | | screenshots |
| `rpc/methods.cpp` | `setmining`, `getmininginfo`; `listtransactions` gains `address`, `getblockchaininfo` `time` | `getmininginfo` | `NodeRpcTest.MiningSwitchedOnAndOff`, `NodeRpcTest.TransactionsShowTheirAddress` |
| `rpc/client.*` | `cli_param` (shared with `bound-cli`) and `split_words` for the console | `rpc/client.cpp` | `WalletApp.ConsoleWordsAreSplitLikeAShell` |

## Choices

- **Qt 6 Widgets** (the project owner's choice, over Dear ImGui and a browser-based wallet): what
  Bitcoin-Qt uses, with native windows on Windows and Linux. On Linux it's `qt6-base-dev` and on by
  default; without it everything but `bound-qt` still builds. On Windows vcpkg builds Qt from
  source (the `gui` feature in `vcpkg.json`): one to two hours, and with 8 parallel compiles it can
  run an 8 GB machine out of memory. So there it's opt-in, with `-DBOUND_GUI=ON`.
- **The app talks to its node over JSON-RPC,** the same as `bound-cli`, even when the node runs
  inside it. It never touches the node's objects across threads: the node keeps its single-threaded
  loop, and the app works the same with a separate `boundd`. Calls go out on one worker thread, in
  order. The answers come back to the GUI thread, and are dropped if the widget that asked has
  gone.
- **Mining is `setmining`,** not a loop of `generatetoaddress` calls. The node's loop owns the
  miner: the RPC only sets who gets paid (`MiningControl`), and the loop starts or stops the
  background miner on its next pass. `setmining true` without an address pays a new address of
  the wallet. As with `--mine`, mining pauses during the initial sync.
- **Settings live beside the wallet** (`<datadir>/bound-qt.ini`), not in the registry: just the
  address the Receive page showed last, checked with `validateaddress` before it's shown again.
- **Secrets:** the console hides the parameters of `walletpassphrase`, `walletpassphrasechange`,
  `createwallet` and `restorewallet`, and keeps them out of its history. The seed dialog clears
  the clipboard if the seed is still on it when the dialog closes.

## Testing

`bound_qt_tests` runs the real app off screen (`QT_QPA_PLATFORM=offscreen`) against a real node
inside it, on a temporary data directory with no seeds:
- create a wallet;
- switch mining on and check that blocks arrive;
- check the Overview and Transactions pages show them;
- close the window and check the node stops.

With `BOUND_SCREENSHOTS=<dir>` it also mines past maturity, sends a payment, and saves a picture
of every page. That's where `docs/screenshots/wallet.png` comes from.
