#include "qt/mainwindow.h"

#include "app/datadir.h"
#include "qt/dialogs.h"
#include "qt/format.h"
#include "qt/icons.h"
#include "qt/style.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include <iterator>
#include <string>
#include <utility>

namespace bound::gui {

namespace {

constexpr int kRefreshMs = 2000;

// Milestone 10: which network, under the name. Mainnet's is green once it has launched; before
// that it's a preview whose coins will be thrown away, and says so in red.
QLabel* network_pill(const ConsensusParams& p, QWidget* parent) {
    const QString name = QString::fromUtf8(p.network_name.data(), static_cast<qsizetype>(p.network_name.size())).toUpper();
    auto* pill = new QLabel(p.launched ? name : name + QStringLiteral(" PREVIEW"), parent);
    pill->setObjectName(QStringLiteral("pill"));
    if (p.network_name == "mainnet") pill->setStyleSheet(p.launched ? QStringLiteral("background: #2b8a3e;") : QStringLiteral("background: #c92a2a;"));
    pill->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return pill;
}

QWidget* brand(const ConsensusParams& params, QWidget* parent) {
    auto* box = new QWidget(parent);
    box->setObjectName(QStringLiteral("brand"));
    auto* layout = new QHBoxLayout(box);
    layout->setContentsMargins(20, 22, 16, 18);
    layout->setSpacing(10);
    auto* mark = new QLabel(box);
    mark->setPixmap(white_mark(34));
    layout->addWidget(mark);
    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    auto* name = new QLabel(QStringLiteral("BOUND"), box);
    name->setObjectName(QStringLiteral("brandName"));
    text->addWidget(name);
    text->addWidget(network_pill(params, box));
    layout->addLayout(text);
    layout->addStretch();
    return box;
}

}  // namespace

MainWindow::MainWindow(NodeHost& host, QWidget* parent)
    : QMainWindow(parent), host_(host), rpc_(cookie_path(host.datadir()), host.rpc_port()), session_(rpc_, host_) {
    setWindowTitle(QStringLiteral("BOUND wallet (%1)").arg(session_.network_name()));
    resize(1080, 700);
    setMinimumSize(900, 600);

    auto* central = new QWidget(this);
    auto* row = new QHBoxLayout(central);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    auto* side = new QWidget(central);
    side->setFixedWidth(210);
    side->setObjectName(QStringLiteral("brand"));
    auto* side_layout = new QVBoxLayout(side);
    side_layout->setContentsMargins(0, 0, 0, 0);
    side_layout->setSpacing(0);
    side_layout->addWidget(brand(session_.params(), side));
    sidebar_ = new QListWidget(side);
    sidebar_->setObjectName(QStringLiteral("sidebar"));
    sidebar_->addItems({QStringLiteral("Overview"), QStringLiteral("Send"), QStringLiteral("Receive"), QStringLiteral("Contacts"),
                        QStringLiteral("Transactions"), QStringLiteral("Mining"), QStringLiteral("Network"), QStringLiteral("Console")});
    sidebar_->setIconSize(QSize(18, 18));
    sidebar_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    set_sidebar_icons();
    QFont side_font = sidebar_->font();
    side_font.setPointSizeF(side_font.pointSizeF() * 1.08);
    sidebar_->setFont(side_font);
    side_layout->addWidget(sidebar_, 1);
    row->addWidget(side);

    auto* content = new QWidget(central);
    content->setObjectName(QStringLiteral("content"));
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(content);
    overview_ = new OverviewPage(session_, stack_);
    send_ = new SendPage(session_, stack_);
    receive_ = new ReceivePage(session_, stack_);
    contacts_ = new ContactsPage(session_, stack_);
    transactions_ = new TransactionsPage(session_, stack_);
    mining_ = new MiningPage(session_, stack_);
    network_ = new NetworkPage(session_, stack_);
    console_ = new ConsolePage(session_, stack_);
    for (QWidget* w : std::initializer_list<QWidget*>{overview_, send_, receive_, contacts_, transactions_, mining_, network_, console_}) {
        stack_->addWidget(w);
    }
    content_layout->addWidget(stack_);
    row->addWidget(content, 1);
    setCentralWidget(central);

    sync_label_ = new QLabel(QStringLiteral("Starting the node..."), this);
    sync_bar_ = new QProgressBar(this);
    sync_bar_->setMaximumWidth(180);
    sync_bar_->setTextVisible(false);
    sync_bar_->hide();
    mining_label_ = new QLabel(this);
    mining_label_->hide();
    peers_label_ = new QLabel(this);
    lock_label_ = new QLabel(this);
    statusBar()->addWidget(sync_label_);
    statusBar()->addWidget(sync_bar_);
    statusBar()->addPermanentWidget(mining_label_);
    statusBar()->addPermanentWidget(peers_label_);
    statusBar()->addPermanentWidget(lock_label_);
    statusBar()->setSizeGripEnabled(false);

    build_menus();

    connect(sidebar_, &QListWidget::currentRowChanged, this, [this](int r) {
        stack_->setCurrentIndex(r);
        refresh();
    });
    sidebar_->setCurrentRow(0);
    connect(&host_, &NodeHost::log_line, console_, &ConsolePage::append_log);
    connect(&host_, &NodeHost::finished, this, [this](const QString& error) {
        if (!error.isEmpty() && !closing_) {
            QMessageBox::critical(this, QStringLiteral("The node stopped"), QStringLiteral("The node stopped: %1").arg(error));
        }
        closing_ = true;
        close();
    });

    session_.refresh = [this] { QTimer::singleShot(150, this, [this] { refresh(); }); };
    // Milestone 18: Overview's buttons and Contacts' Pay.
    session_.pay = [this](const QString& address) {
        send_->set_address(address);
        show_page(Send);
    };
    session_.receive = [this] { show_page(Receive); };
    // Milestone 18: when Windows switches between light and dark, what the pages painted themselves.
    on_theme_change(this, [this] {
        set_sidebar_icons();
        overview_->restyle();
        transactions_->fill();
    });
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::refresh);
    timer_->start(kRefreshMs);
    QTimer::singleShot(0, this, [this] { refresh(); });
}

void MainWindow::build_menus() {
    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("&Back up the 24 words..."), this, [this] { backup_seed(session_, this); });
    file->addAction(QStringLiteral("&Change the passphrase..."), this, [this] { change_passphrase(session_, this); });
    file->addAction(QStringLiteral("&Lock the wallet now"), this, [this] {
        rpc_.call("walletlock", json::array(), this, [this](const json&) { refresh(); });
    });
    file->addSeparator();
    file->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, &QWidget::close);
    QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About BOUND"), this, [this] { show_about(this); });
    help->addAction(QStringLiteral("About &Qt"), qApp, &QApplication::aboutQt);
}

void MainWindow::set_sidebar_icons() {
    const Glyph glyphs[] = {Glyph::Overview, Glyph::Send,    Glyph::Receive, Glyph::Contacts,
                            Glyph::Transactions, Glyph::Mining, Glyph::Network, Glyph::Console};
    for (int i = 0; i < sidebar_->count() && i < static_cast<int>(std::size(glyphs)); ++i) {
        sidebar_->item(i)->setIcon(glyph_icon(glyphs[i], colors().sidebar_text, Qt::white));
    }
}

void MainWindow::show_page(Page p) { sidebar_->setCurrentRow(static_cast<int>(p)); }

QWidget* MainWindow::page(Page p) const { return stack_->widget(static_cast<int>(p)); }

void MainWindow::request(const std::string& method, json params, std::function<void(const json&)> done,
                         std::function<void(const RpcFailure&)> failed) {
    ++pending_;
    rpc_.call(
        method, std::move(params), this,
        [this, done = std::move(done)](const json& result) {
            reachable_ = true;
            if (done) done(result);
            if (--pending_ == 0) finished_batch();
        },
        [this, failed = std::move(failed)](const RpcFailure& f) {
            if (!f.unreachable) reachable_ = true;
            if (failed) failed(f);
            if (--pending_ == 0) finished_batch();
        });
}

void MainWindow::refresh() {
    if (pending_ > 0 || closing_) return;
    reachable_ = false;
    request("getblockchaininfo", json::array(), [this](const json& j) {
        status_.blocks = int_of(j, "blocks");
        status_.headers = int_of(j, "headers");
        status_.tip_time = int_of(j, "time");
    });
    request("getnetworkinfo", json::array(), [this](const json& j) {
        status_.peers = int_of(j, "connections");
        network_->show_network(j);
    });
    request("getmininginfo", json::array(), [this](const json& j) {
        mining_info_ = j;
        mining_->show_mining(j);
    });
    request(
        "getwalletinfo", json::array(),
        [this](const json& info) {
            session_.set_wallet(info);
            overview_->show_wallet(info);
            send_->show_wallet(info);
            receive_->show_wallet(info);
            // The list again only when something in it may have changed: confirmations, new payments.
            const std::string key = std::to_string(status_.blocks) + "/" + info.dump();
            if (key == tx_key_) return;
            request("listtransactions", json::array({1000}), [this, key](const json& list) {
                tx_key_ = key;
                overview_->show_transactions(list);
                transactions_->show_transactions(list);
            });
        },
        [this](const RpcFailure& f) {
            if (f.code != rpc_error::kNoWallet) return;
            session_.set_wallet(json());
            tx_key_.clear();
            overview_->show_wallet(json());
            send_->show_wallet(json());
            receive_->show_wallet(json());
            overview_->show_transactions(json::array());
            transactions_->show_transactions(json::array());
            if (!offered_wallet_) {
                offered_wallet_ = true;
                QTimer::singleShot(0, this, [this] {
                    if (!session_.has_wallet()) offer_wallet(session_, this);
                });
            }
        });
    if (stack_->currentWidget() == network_) {
        request("getpeerinfo", json::array(), [this](const json& j) { network_->show_peers(j); });
    }
    if (stack_->currentWidget() == send_) {
        request("estimatefee", json::array(), [this](const json& j) {
            if (j.is_number()) send_->show_fee_estimate(j.get<double>());
        });
    }
    if (stack_->currentWidget() == mining_) {
        request("getnetworkhashps", json::array(), [this](const json& j) {
            if (j.is_number()) mining_->show_network_hashrate(j.get<double>());
        });
    }
}

void MainWindow::finished_batch() {
    if (closing_) return;
    if (!reachable_) {
        sync_label_->setText(connected_ ? QStringLiteral("The node isn't answering") : QStringLiteral("Starting the node..."));
        sync_bar_->hide();
        return;
    }
    connected_ = true;
    update_status();
    overview_->show_status(status_, mining_info_);
}

void MainWindow::update_status() {
    if (status_.headers > status_.blocks) {
        sync_label_->setText(QStringLiteral("Catching up: block %1 of %2").arg(status_.blocks).arg(status_.headers));
        sync_bar_->setRange(0, static_cast<int>(status_.headers));
        sync_bar_->setValue(static_cast<int>(status_.blocks));
        sync_bar_->show();
    } else {
        sync_label_->setText(status_.peers == 0 ? QStringLiteral("No peers yet: looking for the network (block #%1)").arg(status_.blocks)
                                                : QStringLiteral("Up to date: block #%1").arg(status_.blocks));
        sync_bar_->hide();
    }
    const QColor dot = status_.peers > 0 ? colors().positive : colors().negative;
    peers_label_->setText(QStringLiteral("<span style='color:%1'>&#9679;</span> %2 %3")
                              .arg(dot.name())
                              .arg(status_.peers)
                              .arg(status_.peers == 1 ? QStringLiteral("peer") : QStringLiteral("peers")));
    const bool mining = mining_info_.value("mining", false);
    mining_label_->setVisible(mining);
    if (mining) mining_label_->setText(QStringLiteral("Mining, %1").arg(hashrate_text(mining_info_.value("hashespersec", 0.0))));
    if (!session_.has_wallet()) {
        lock_label_->setText(QStringLiteral("No wallet"));
    } else {
        lock_label_->setText(session_.wallet().value("locked", true) ? QStringLiteral("Wallet locked") : QStringLiteral("Wallet unlocked"));
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (host_.embedded() && host_.running()) {
        // Close once the node inside has stopped and saved (NodeHost::finished).
        if (!closing_) {
            closing_ = true;
            timer_->stop();
            centralWidget()->setEnabled(false);
            sync_label_->setText(QStringLiteral("Stopping the node..."));
            host_.stop();
        }
        event->ignore();
        return;
    }
    closing_ = true;
    event->accept();
}

}  // namespace bound::gui
