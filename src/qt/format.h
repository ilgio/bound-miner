#pragma once

// How the wallet app shows the node's answers: amounts, times, hashes, transaction kinds.

#include "core/amount.h"
#include "rpc/server.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <string_view>

namespace bound::gui {

QString text_of(const json& j, const char* key);           // a string or number field as text; "" if missing
std::int64_t int_of(const json& j, const char* key, std::int64_t fallback = 0);
std::optional<Amount> amount_of(const json& j, const char* key);  // an RPC amount ("1.50000000")

// "1.50 BOUND", "0.00000191 BOUND": at least two decimals, no trailing zeros past them (milestone
// 18); with `sign`, positive amounts get a "+". amount_text is the number alone.
QString coins_text(Amount amount, bool sign = false);
QString amount_text(Amount amount);
QString date_text(std::int64_t unix_time);    // local time, "2026-11-15 11:38"
QString age_text(std::int64_t seconds);       // "40 s", "3 min", "5 h", "2 days"
QString short_hash(const QString& hash);      // "0000a7ba…79293"
QString hashrate_text(double hashes_per_second);
// listtransactions' category as the app shows it: Received, Sent, Mined, ...
QString category_text(const QString& category);

// Milestone 18: payment links, as Bitcoin's bitcoin: URIs (BIP21): "bound:<address>" and
// optionally "?amount=<BOUND>". The Receive page's QR code holds one when an amount is asked for;
// pasted into Send, it fills the address and the amount.
struct PaymentRequest {
    QString address;
    std::optional<Amount> amount;
};
QString payment_link(const QString& address, std::optional<Amount> amount = std::nullopt);
std::optional<PaymentRequest> parse_payment_link(const QString& text);  // nullopt unless a valid bound: link

// The public explorer's page for a transaction (mainnet's and testnet's; regtest has none).
std::optional<QString> explorer_tx_url(std::string_view network, const QString& txid);

}  // namespace bound::gui
