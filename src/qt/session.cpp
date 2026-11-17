#include "qt/session.h"

#include "core/address.h"
#include "qt/dialogs.h"
#include "qt/nodehost.h"

#include <QMessageBox>
#include <QPointer>
#include <QWidget>

#include <optional>
#include <utility>

namespace bound::gui {

Session::Session(Rpc& rpc, NodeHost& host, QObject* parent)
    : QObject(parent), rpc_(rpc), host_(host), contacts_(settings_file()) {}

QString Session::who(const QString& address) const {
    if (const std::optional<QString> name = contacts_.name_for(address)) return QStringLiteral("%1 (%2)").arg(*name, address);
    return address;
}

bool Session::valid_address(const QString& address) const {
    return decode_address(address.trimmed().toStdString(), params().address_version).has_value();
}

QString Session::settings_file() const {
    return QString::fromStdU16String((host_.datadir() / "bound-qt.ini").u16string());
}

const ConsensusParams& Session::params() const { return host_.params(); }

QString Session::network_name() const {
    const std::string_view name = host_.params().network_name;
    return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}

void Session::with_unlocked(QWidget* parent, const QString& purpose, std::function<void(Finish finish)> action) {
    const bool locked = !has_wallet() || wallet_.value("locked", true);
    if (!locked) {
        action([] {});
        return;
    }
    std::optional<QString> passphrase = ask_passphrase(parent, purpose);
    if (!passphrase) return;
    QPointer<QWidget> owner(parent);
    rpc_.call(
        "walletpassphrase", json::array({passphrase->toStdString(), 300}), this,
        [this, action](const json&) {
            action([this] {
                rpc_.call("walletlock", json::array(), this, [this](const json&) { refresh(); });
            });
        },
        [this, owner, purpose, action](const RpcFailure& failure) {
            if (failure.code == rpc_error::kWrongPassphrase) {
                QMessageBox::warning(owner, QStringLiteral("Wrong passphrase"), QStringLiteral("That passphrase doesn't unlock this wallet."));
                with_unlocked(owner, purpose, action);
                return;
            }
            show_error(owner, QStringLiteral("Can't unlock the wallet"), failure);
        });
    passphrase->fill(QLatin1Char(' '));
}

void Session::show_error(QWidget* parent, const QString& title, const RpcFailure& failure) {
    QString text = failure.message;
    if (failure.unreachable) text = QStringLiteral("The node isn't answering: ") + text;
    QMessageBox::warning(parent, title, text);
}

}  // namespace bound::gui
