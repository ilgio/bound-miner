#include "qt/format.h"

#include "core/params.h"

#include <QDateTime>

#include <string>

namespace bound::gui {

QString text_of(const json& j, const char* key) {
    if (!j.is_object()) return {};
    const auto found = j.find(key);
    if (found == j.end() || found->is_null()) return {};
    if (found->is_string()) return QString::fromStdString(found->get<std::string>());
    return QString::fromStdString(found->dump());
}

std::int64_t int_of(const json& j, const char* key, std::int64_t fallback) {
    if (!j.is_object()) return fallback;
    const auto found = j.find(key);
    if (found == j.end() || !found->is_number()) return fallback;
    return found->get<std::int64_t>();
}

std::optional<Amount> amount_of(const json& j, const char* key) {
    if (!j.is_object()) return std::nullopt;
    const auto found = j.find(key);
    if (found == j.end() || !found->is_string()) return std::nullopt;
    return parse_coins(found->get<std::string>());
}

QString amount_text(Amount amount) {
    QString text = QString::fromStdString(format_coins(amount));
    // Milestone 18: no zeros after the second decimal that say nothing: 5.00, 1.50, 0.00000191.
    const qsizetype point = text.indexOf(QLatin1Char('.'));
    if (point >= 0) {
        while (text.size() > point + 3 && text.endsWith(QLatin1Char('0'))) text.chop(1);
    }
    return text;
}

QString coins_text(Amount amount, bool sign) {
    QString text = amount_text(amount);
    if (sign && amount > 0) text.prepend(QLatin1Char('+'));
    return text + QLatin1Char(' ') + QString::fromUtf8(kTicker.data(), static_cast<qsizetype>(kTicker.size()));
}

QString payment_link(const QString& address, std::optional<Amount> amount) {
    QString link = QStringLiteral("bound:") + address;
    if (amount) {
        QString number = QString::fromStdString(format_coins(*amount));
        while (number.endsWith(QLatin1Char('0'))) number.chop(1);
        if (number.endsWith(QLatin1Char('.'))) number.chop(1);
        link += QStringLiteral("?amount=") + number;
    }
    return link;
}

std::optional<PaymentRequest> parse_payment_link(const QString& text) {
    const QString link = text.trimmed();
    if (!link.startsWith(QStringLiteral("bound:"), Qt::CaseInsensitive)) return std::nullopt;
    const QString rest = link.mid(6);
    const qsizetype query = rest.indexOf(QLatin1Char('?'));
    PaymentRequest request;
    request.address = query < 0 ? rest : rest.left(query);
    if (request.address.isEmpty()) return std::nullopt;
    if (query >= 0) {
        for (const QString& pair : rest.mid(query + 1).split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
            const qsizetype eq = pair.indexOf(QLatin1Char('='));
            const QString key = (eq < 0 ? pair : pair.left(eq)).toLower();
            const QString value = eq < 0 ? QString() : pair.mid(eq + 1);
            if (key == QLatin1String("amount")) {
                const std::optional<Amount> amount = parse_coins(value.toStdString());
                if (!amount || *amount <= 0) return std::nullopt;
                request.amount = amount;
            } else if (key.startsWith(QLatin1String("req-"))) {
                return std::nullopt;  // as BIP21: a required parameter this app doesn't know
            }
        }
    }
    return request;
}

std::optional<QString> explorer_tx_url(std::string_view network, const QString& txid) {
    if (network == "mainnet") return QStringLiteral("https://explorer.bound.sarl/tx/") + txid;
    if (network == "testnet") return QStringLiteral("https://testnet-explorer.bound.sarl/tx/") + txid;
    return std::nullopt;
}

QString date_text(std::int64_t unix_time) {
    return QDateTime::fromSecsSinceEpoch(unix_time).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString age_text(std::int64_t seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds < 90) return QStringLiteral("%1 s").arg(seconds);
    if (seconds < 90 * 60) return QStringLiteral("%1 min").arg(seconds / 60);
    if (seconds < 36 * 3600) return QStringLiteral("%1 h").arg(seconds / 3600);
    return QStringLiteral("%1 days").arg(seconds / 86400);
}

QString short_hash(const QString& hash) {
    if (hash.size() <= 16) return hash;
    return hash.left(8) + QChar(0x2026) + hash.right(5);
}

QString hashrate_text(double h) {
    if (h >= 1e9) return QStringLiteral("%1 GH/s").arg(h / 1e9, 0, 'f', 2);
    if (h >= 1e6) return QStringLiteral("%1 MH/s").arg(h / 1e6, 0, 'f', 2);
    if (h >= 1e3) return QStringLiteral("%1 kH/s").arg(h / 1e3, 0, 'f', 1);
    return QStringLiteral("%1 H/s").arg(h, 0, 'f', 0);
}

QString category_text(const QString& category) {
    if (category == QLatin1String("send")) return QStringLiteral("Sent");
    if (category == QLatin1String("receive")) return QStringLiteral("Received");
    if (category == QLatin1String("generate")) return QStringLiteral("Mined");
    if (category == QLatin1String("immature")) return QStringLiteral("Mined (maturing)");
    if (category == QLatin1String("orphan")) return QStringLiteral("Mined (lost to a reorg)");
    return category;
}

}  // namespace bound::gui
