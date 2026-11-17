# Milestone 18: an installer and a better wallet app

**What it adds:** `bound-<version>-win64-setup.exe`, an installer for Windows, and a wallet app that's
easier to use: a fresh look, a QR code on Receive, contacts and simpler sending, and more in a
transaction's details.

## Decided (2026-11-16, by the project owner)

"Make a good application with exe and good UI", and all four of the improvements offered: a fresh
look, a QR code on Receive, easier sending, transaction details. Asked why the app offers new
addresses, the Receive page now says why.

## The installer

- **Inno Setup 6** builds it in the release workflow from the same folder the zip holds
  (`contrib/release/bound.iss`), and `package-windows.ps1` then tries it: installs it for the
  current user into a folder of its own, runs `boundd --version` from there, uninstalls it and
  checks the files are gone. Locally the script makes it only when Inno Setup is installed.
- It installs into Program Files for everyone with admin rights, or into the user's own programs
  folder without; adds **BOUND** and **BOUND (testnet)** to the Start menu, a desktop icon if asked,
  and an uninstaller. A newer version installs over an older one, asking to close a running BOUND
  first. Uninstalling leaves the chain and the wallet in `%APPDATA%\BOUND`.
- Like the programs, it isn't code-signed yet, so Windows warns before running it.

## The wallet app

- **Fresh look:** line icons drawn in code (sidebar, transactions), Send and Receive buttons under
  the balance, recent transactions as rows that open their details, amounts without zeros that say
  nothing (`5.00`, `1.50`, `0.00000191`), and light or dark following Windows as it switches.
- **Receive:** the address as a QR code, which phones read as text; an optional amount turns it
  into a payment link, `bound:<address>?amount=<BOUND>`, the way Bitcoin's `bitcoin:` links work
  (BIP21). A note says why there are new addresses. Fixed: the Copy button only worked for testnet
  addresses (it checked for a `9`).
- **Contacts** (a new page): names for addresses, kept in the app's settings file beside the chain,
  apart from the wallet. Send has a Contacts menu, recognises a saved address, and offers to save
  a new one after paying it; Transactions and Overview show names.
- **Easier sending:** a pasted payment link fills the address and the amount; **Max** sends everything
  spendable with the fee taken out (the new `sendall` RPC, `Wallet::create_sweep`: every spendable
  coin, biggest first, as many as fit in a standard transaction, one output); the fee is **Slow** (1
  unit a byte, the least relayed), **Normal** (what makes the next block now), **Fast** (twice that,
  at least 5) or Custom, each with what a typical payment would pay.
- **Transaction details:** open from Overview with a click, or Transactions with a double-click or
  Enter; they show the contact and the memo, and **Open in explorer** goes to the transaction on
  explorer.bound.sarl (mainnet) or testnet-explorer.bound.sarl. Transactions can be searched by name,
  address, ID, amount or date.

## In the code

- `qt/icons.*` (the line icons), `qt/qr.*` and `qt/qrcodegen/` (Project Nayuki's QR Code generator
  1.8.0, MIT, unchanged; its license ships in `licenses/`), `qt/contacts.*`, `qt/contactspage.cpp`.
- `qt/format.*`: `amount_text`, `payment_link`, `parse_payment_link`, `explorer_tx_url`.
- `Session::pay`, `Session::receive`, `Session::contacts()`, `on_theme_change` (style).
- RPC `sendall <address> [fee_rate] [memo]`, also in `bound-cli`.

## Tests

`sendall` and `create_sweep` (two coins, one output, the fee exact, nothing left after); payment
links both ways and the links refused; contacts saved, renamed, removed and read back; QR codes
against the parts every QR code has (size, finder squares, timing lines, the format bits for level
M under their BCH code) and the picture's white border; every icon drawn; amounts as shown.
