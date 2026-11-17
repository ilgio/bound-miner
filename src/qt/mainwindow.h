#pragma once

// The wallet app's window: pages on the left, like Bitcoin-Qt's tabs, and the node's state in the
// status bar. It asks the node for everything every two seconds.

#include "qt/nodehost.h"
#include "qt/pages.h"
#include "qt/rpc.h"
#include "qt/session.h"

#include <QMainWindow>

#include <memory>

class QCloseEvent;
class QLabel;
class QListWidget;
class QProgressBar;
class QStackedWidget;
class QTimer;

namespace bound::gui {

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(NodeHost& host, QWidget* parent = nullptr);

    // For tests: the pages, in sidebar order, and whether the node has answered yet.
    enum Page { Overview, Send, Receive, Contacts, Transactions, Mining, Network, Console };
    void show_page(Page page);
    QWidget* page(Page page) const;
    bool connected() const { return connected_; }
    void refresh();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void build_menus();
    void request(const std::string& method, json params, std::function<void(const json&)> done,
                 std::function<void(const RpcFailure&)> failed = {});
    void finished_batch();
    void update_status();
    void set_sidebar_icons();  // milestone 18

    NodeHost& host_;
    Rpc rpc_;
    Session session_;

    QListWidget* sidebar_;
    QStackedWidget* stack_;
    OverviewPage* overview_;
    SendPage* send_;
    ReceivePage* receive_;
    ContactsPage* contacts_;
    TransactionsPage* transactions_;
    MiningPage* mining_;
    NetworkPage* network_;
    ConsolePage* console_;

    QLabel* sync_label_;
    QProgressBar* sync_bar_;
    QLabel* peers_label_;
    QLabel* mining_label_;
    QLabel* lock_label_;
    QTimer* timer_;

    int pending_ = 0;         // calls of this refresh still out
    bool reachable_ = false;  // this refresh got an answer
    bool connected_ = false;  // the node has answered since the app started
    bool offered_wallet_ = false;
    bool closing_ = false;
    ChainStatus status_;
    json mining_info_ = json::object();
    std::string tx_key_;  // the wallet's state the transaction list was fetched for
};

}  // namespace bound::gui
