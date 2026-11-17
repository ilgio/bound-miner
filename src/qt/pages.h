#pragma once

// The wallet app's pages. Each shows what the main window hands it from the node's answers
// (show_*), and acts through the session.

#include "core/amount.h"
#include "qt/session.h"

#include <QLabel>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <string>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QScrollArea;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QToolButton;
class QVBoxLayout;

namespace bound::gui {

// The chain and network as the status bar and overview need them.
struct ChainStatus {
    std::int64_t blocks = 0;
    std::int64_t headers = 0;
    std::int64_t tip_time = 0;
    std::int64_t peers = 0;
};

class OverviewPage : public QWidget {
public:
    explicit OverviewPage(Session& session, QWidget* parent = nullptr);
    void show_wallet(const json& info);  // null: no wallet
    void show_transactions(const json& list);
    void show_status(const ChainStatus& status, const json& mining);
    void restyle();  // milestone 18: after the theme or a contact's name changed

private:
    Session& session_;
    QLabel* available_;
    QLabel* pending_;
    QLabel* immature_;
    QLabel* total_;
    QWidget* balances_;
    QWidget* no_wallet_;
    QLabel* height_;
    QLabel* peers_;
    QLabel* sync_;
    QLabel* mining_;
    QVBoxLayout* recent_;
    QScrollArea* scroll_;
    QWidget* rows_ = nullptr;  // the recent transactions, rebuilt on each change
    std::string shown_;        // what rows_ shows, to skip rebuilding it for nothing
    json list_ = json::array();
};

class SendPage : public QWidget {
public:
    explicit SendPage(Session& session, QWidget* parent = nullptr);
    void show_wallet(const json& info);
    void show_fee_estimate(double units_per_byte);
    void set_address(const QString& address);  // milestone 18: from Contacts' Pay

private:
    void check();
    void send();
    void fill_contacts_menu();
    void save_contact(const QString& address);
    std::optional<double> chosen_rate() const;  // units per byte; nullopt: the node's own choice

    Session& session_;
    QLineEdit* address_;
    QToolButton* contacts_;
    QLabel* address_note_;
    QLineEdit* amount_;
    QPushButton* max_;
    QLabel* available_;
    QLineEdit* memo_;
    QRadioButton* slow_;
    QRadioButton* normal_;
    QRadioButton* fast_;
    QRadioButton* custom_fee_;
    QLineEdit* fee_rate_;
    QLabel* fee_note_;
    QPushButton* send_;
    std::optional<double> estimate_;
    Amount spendable_ = 0;
};

class ReceivePage : public QWidget {
public:
    explicit ReceivePage(Session& session, QWidget* parent = nullptr);
    void show_wallet(const json& info);

private:
    void new_address();
    void show_address(const QString& address);
    void update_code();  // the QR code and the link, for the address and any amount asked for

    Session& session_;
    QLabel* code_;
    QLabel* address_;
    QPushButton* copy_;
    QPushButton* new_;
    QLineEdit* request_;
    QPushButton* copy_link_;
    QLabel* note_;
    QString shown_;  // the address on show, "" while there's none
    bool asked_ = false;
};

// Milestone 18: names for the addresses you pay.
class ContactsPage : public QWidget {
public:
    explicit ContactsPage(Session& session, QWidget* parent = nullptr);
    void fill();

private:
    std::optional<Contact> selected() const;
    void edit(std::optional<Contact> contact);

    Session& session_;
    QTableWidget* table_;
    QLabel* empty_;
    QPushButton* pay_;
    QPushButton* edit_;
    QPushButton* delete_;
};

class TransactionsPage : public QWidget {
public:
    explicit TransactionsPage(Session& session, QWidget* parent = nullptr);
    void show_transactions(const json& list);
    void fill();

private:
    Session& session_;
    QComboBox* filter_;
    QLineEdit* search_;
    QTableWidget* table_;
    QLabel* empty_;
    json list_ = json::array();
    int contacts_version_ = -1;
};

class MiningPage : public QWidget {
public:
    explicit MiningPage(Session& session, QWidget* parent = nullptr);
    void show_mining(const json& info);
    void show_network_hashrate(double hashes_per_second);

private:
    void toggle();

    Session& session_;
    QPushButton* toggle_;
    QLineEdit* payee_;
    QSpinBox* threads_;  // milestone 12
    QLabel* status_;
    QLabel* rate_;
    QLabel* found_;
    QLabel* difficulty_;
    QLabel* network_rate_;
    QLabel* paying_;
    bool mining_ = false;
};

class NetworkPage : public QWidget {
public:
    explicit NetworkPage(Session& session, QWidget* parent = nullptr);
    void show_network(const json& info);
    void show_peers(const json& list);

private:
    Session& session_;
    QLabel* counts_;
    QLabel* port_;
    QLabel* reachable_;
    QLabel* version_;
    QTableWidget* peers_;
};

class ConsolePage : public QWidget {
public:
    explicit ConsolePage(Session& session, QWidget* parent = nullptr);
    void append_log(const QString& line);
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void run(const QString& line);
    void print(const QString& html);

    Session& session_;
    QTabWidget* tabs_;
    QPlainTextEdit* output_;
    QLineEdit* input_;
    QPlainTextEdit* log_;
    QStringList history_;
    int history_pos_ = 0;
};

}  // namespace bound::gui
