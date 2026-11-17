#pragma once

// What every page of the wallet app shares: the connection to the node, the wallet's state as
// last seen, and unlocking the wallet for a moment to spend.

#include "core/params.h"
#include "qt/contacts.h"
#include "qt/rpc.h"

#include <QObject>
#include <QString>

#include <functional>
#include <utility>

class QWidget;

namespace bound::gui {

class NodeHost;

class Session : public QObject {
public:
    Session(Rpc& rpc, NodeHost& host, QObject* parent = nullptr);

    Rpc& rpc() { return rpc_; }
    NodeHost& host() { return host_; }
    // The app's own settings, kept in the data directory beside the wallet they belong to.
    QString settings_file() const;
    // Milestone 10: the network's rules (address format) and name ("testnet", "mainnet", ...).
    const ConsensusParams& params() const;
    QString network_name() const;

    // getwalletinfo as last seen; null while there's no wallet (or before the first answer).
    const json& wallet() const { return wallet_; }
    bool has_wallet() const { return wallet_.is_object(); }
    void set_wallet(json info) { wallet_ = std::move(info); }

    // Runs `action` with the wallet unlocked. If it's locked, asks for the passphrase first (saying
    // what it's for), and locks it again when `action` calls the `finish` it's given. If the user
    // unlocked it already (walletpassphrase in the console), it's left that way.
    using Finish = std::function<void()>;
    void with_unlocked(QWidget* parent, const QString& purpose, std::function<void(Finish finish)> action);

    // Refreshes every page now, rather than at the next tick (set by the main window).
    std::function<void()> refresh = [] {};
    // Milestone 18 (set by the main window): opens Send with this address filled in, or Receive.
    std::function<void(const QString& address)> pay = [](const QString&) {};
    std::function<void()> receive = [] {};

    // Milestone 18: names for addresses, the app's own (not the wallet's).
    Contacts& contacts() { return contacts_; }
    const Contacts& contacts() const { return contacts_; }
    // The contact's name and the address, or the address alone.
    QString who(const QString& address) const;
    // Whether `address` is an address on this node's network.
    bool valid_address(const QString& address) const;

    static void show_error(QWidget* parent, const QString& title, const RpcFailure& failure);

private:
    Rpc& rpc_;
    NodeHost& host_;
    json wallet_;
    Contacts contacts_;
};

}  // namespace bound::gui
