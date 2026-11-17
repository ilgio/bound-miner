#include "qt/dialogs.h"
#include "qt/format.h"
#include "qt/icons.h"
#include "qt/pages.h"
#include "qt/style.h"

#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace bound::gui {

namespace {

QLabel* card_title(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text.toUpper(), parent);
    label->setObjectName(QStringLiteral("cardTitle"));
    return label;
}

QLabel* value_label(QWidget* parent, const char* object_name = nullptr) {
    auto* label = new QLabel(QStringLiteral("-"), parent);
    if (object_name != nullptr) label->setObjectName(QString::fromLatin1(object_name));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

// Milestone 18: one recent transaction, which opens its details when clicked.
class TxRow : public QFrame {
public:
    TxRow(std::function<void()> open, QWidget* parent) : QFrame(parent), open_(std::move(open)) {
        setObjectName(QStringLiteral("txRow"));
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Details"));
    }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) open_();
        QFrame::mouseReleaseEvent(event);
    }

private:
    std::function<void()> open_;
};

Glyph glyph_for(const QString& category) {
    if (category == QLatin1String("send")) return Glyph::Sent;
    if (category == QLatin1String("receive")) return Glyph::Received;
    return Glyph::Mined;
}

}  // namespace

OverviewPage::OverviewPage(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(28, 24, 28, 24);
    page->setSpacing(16);
    page->addWidget(page_title(QStringLiteral("Overview"), this));

    auto* top = new QHBoxLayout;
    top->setSpacing(16);

    // Balances, or the offer of a wallet.
    QFrame* money = card(this);
    auto* money_layout = new QVBoxLayout(money);
    money_layout->setContentsMargins(22, 18, 22, 18);
    balances_ = new QWidget(money);
    auto* b = new QGridLayout(balances_);
    b->setContentsMargins(0, 0, 0, 0);
    b->setVerticalSpacing(6);
    b->addWidget(card_title(QStringLiteral("Available"), balances_), 0, 0, 1, 2);
    available_ = value_label(balances_, "bigAmount");
    b->addWidget(available_, 1, 0, 1, 2);
    pending_ = value_label(balances_);
    immature_ = value_label(balances_);
    total_ = value_label(balances_, "mediumValue");
    b->addWidget(muted_label(QStringLiteral("Pending"), balances_), 2, 0);
    b->addWidget(pending_, 2, 1, Qt::AlignRight);
    QLabel* maturing = muted_label(QStringLiteral("Maturing"), balances_);
    maturing->setToolTip(QStringLiteral("Mined coins: spendable once 100 more blocks are on top of theirs"));
    b->addWidget(maturing, 3, 0);
    b->addWidget(immature_, 3, 1, Qt::AlignRight);
    b->addWidget(muted_label(QStringLiteral("Total"), balances_), 4, 0);
    b->addWidget(total_, 4, 1, Qt::AlignRight);
    // Milestone 18: the two things people come to the wallet for.
    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto* send = new QPushButton(QStringLiteral("Send"), balances_);
    send->setObjectName(QStringLiteral("primary"));
    send->setIcon(glyph_icon(Glyph::Send, Qt::white, Qt::white, 16));
    auto* receive = new QPushButton(QStringLiteral("Receive"), balances_);
    receive->setIcon(glyph_icon(Glyph::Receive, colors().text, colors().text, 16));
    actions->addWidget(send);
    actions->addWidget(receive);
    actions->addStretch();
    b->addLayout(actions, 5, 0, 1, 2);
    b->setRowMinimumHeight(5, 44);
    connect(send, &QPushButton::clicked, this, [this] { session_.pay(QString()); });
    connect(receive, &QPushButton::clicked, this, [this] { session_.receive(); });
    on_theme_change(receive, [receive] { receive->setIcon(glyph_icon(Glyph::Receive, colors().text, colors().text, 16)); });
    money_layout->addWidget(balances_);

    no_wallet_ = new QWidget(money);
    auto* n = new QVBoxLayout(no_wallet_);
    n->setContentsMargins(0, 0, 0, 0);
    n->addWidget(card_title(QStringLiteral("Wallet"), no_wallet_));
    auto* none = new QLabel(QStringLiteral("No wallet yet"), no_wallet_);
    none->setObjectName(QStringLiteral("mediumValue"));
    n->addWidget(none);
    n->addWidget(muted_label(QStringLiteral("Create one to get an address and receive coins, or restore one from its 24 words."), no_wallet_));
    auto* buttons = new QHBoxLayout;
    auto* create = new QPushButton(QStringLiteral("Create a wallet"), no_wallet_);
    create->setObjectName(QStringLiteral("primary"));
    auto* restore = new QPushButton(QStringLiteral("Restore from 24 words"), no_wallet_);
    buttons->addWidget(create);
    buttons->addWidget(restore);
    buttons->addStretch();
    n->addLayout(buttons);
    n->addStretch();
    connect(create, &QPushButton::clicked, this, [this] { create_wallet(session_, this); });
    connect(restore, &QPushButton::clicked, this, [this] { restore_wallet(session_, this); });
    money_layout->addWidget(no_wallet_);
    balances_->hide();
    top->addWidget(money, 3);

    // The node.
    QFrame* node = card(this);
    auto* g = new QGridLayout(node);
    g->setContentsMargins(22, 18, 22, 18);
    g->setVerticalSpacing(8);
    g->addWidget(card_title(QStringLiteral("Network"), node), 0, 0, 1, 2);
    height_ = value_label(node);
    peers_ = value_label(node);
    sync_ = value_label(node);
    mining_ = value_label(node);
    g->addWidget(muted_label(QStringLiteral("Height"), node), 1, 0);
    g->addWidget(height_, 1, 1, Qt::AlignRight);
    g->addWidget(muted_label(QStringLiteral("Peers"), node), 2, 0);
    g->addWidget(peers_, 2, 1, Qt::AlignRight);
    g->addWidget(muted_label(QStringLiteral("Chain"), node), 3, 0);
    g->addWidget(sync_, 3, 1, Qt::AlignRight);
    g->addWidget(muted_label(QStringLiteral("Mining"), node), 4, 0);
    g->addWidget(mining_, 4, 1, Qt::AlignRight);
    g->setRowStretch(5, 1);
    top->addWidget(node, 2);
    page->addLayout(top);

    // The latest transactions.
    QFrame* recent = card(this);
    recent_ = new QVBoxLayout(recent);
    recent_->setContentsMargins(16, 18, 16, 14);
    recent_->setSpacing(4);
    QLabel* recent_title = card_title(QStringLiteral("Recent transactions"), recent);
    recent_title->setContentsMargins(6, 0, 0, 4);
    recent_->addWidget(recent_title);
    // The rows scroll rather than squeeze when the window is short.
    scroll_ = new QScrollArea(recent);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    recent_->addWidget(scroll_, 1);
    show_transactions(json::array());
    page->addWidget(recent, 1);
}

void OverviewPage::show_wallet(const json& info) {
    const bool have = info.is_object();
    balances_->setVisible(have);
    no_wallet_->setVisible(!have);
    if (!have) return;
    const Amount spendable = amount_of(info, "balance").value_or(0);
    const Amount pending = amount_of(info, "unconfirmed_balance").value_or(0);
    const Amount immature = amount_of(info, "immature_balance").value_or(0);
    available_->setText(coins_text(spendable));
    pending_->setText(coins_text(pending));
    immature_->setText(coins_text(immature));
    total_->setText(coins_text(spendable + pending + immature));
}

void OverviewPage::restyle() {
    shown_.clear();
    show_transactions(list_);
}

void OverviewPage::show_transactions(const json& list) {
    const std::string key = list.dump() + "/" + std::to_string(session_.contacts().version());
    if (rows_ != nullptr && key == shown_) return;
    shown_ = key;
    list_ = list.is_array() ? list : json::array();
    rows_ = new QWidget;  // the scroll area deletes the one before when it takes this
    auto* column = new QVBoxLayout(rows_);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    int count = 0;
    for (auto it = list_.rbegin(); it != list_.rend() && count < 12; ++it, ++count) {
        const json& tx = *it;
        const QString category = text_of(tx, "category");
        const QString txid = text_of(tx, "txid");
        auto* row = new TxRow([this, txid] { show_transaction(session_, this, txid); }, rows_);
        row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(6, 6, 6, 6);
        line->setSpacing(12);
        const Amount amount = amount_of(tx, "amount").value_or(0);
        const QColor tone = category == QLatin1String("send") ? colors().negative
                            : category == QLatin1String("receive") ? colors().positive
                                                                   : colors().accent;
        auto* icon = new QLabel(row);
        icon->setPixmap(glyph_pixmap(glyph_for(category), tone, 20));
        icon->setFixedSize(28, 28);
        icon->setAlignment(Qt::AlignCenter);
        icon->setStyleSheet(QStringLiteral("background: %1; border-radius: 14px;").arg(colors().hover.name()));
        line->addWidget(icon);

        auto* what = new QVBoxLayout;
        what->setSpacing(2);
        auto* type = new QLabel(category_text(category), row);
        type->setStyleSheet(QStringLiteral("font-weight: 600;"));
        const std::int64_t confirmations = int_of(tx, "confirmations");
        auto* when = muted_label(date_text(int_of(tx, "time")) + (confirmations == 0 ? QStringLiteral("  ·  unconfirmed") : QString()), row);
        when->setWordWrap(false);
        what->addWidget(type);
        what->addWidget(when);
        line->addLayout(what);

        const QString address = text_of(tx, "address");
        const std::optional<QString> name = session_.contacts().name_for(address);
        auto* who = muted_label(name ? *name : short_hash(address), row);
        who->setWordWrap(false);
        if (!name) who->setFont(mono_font());
        line->addWidget(who, 1, Qt::AlignRight | Qt::AlignVCenter);

        auto* value = new QLabel(coins_text(amount, true), row);
        value->setMinimumWidth(150);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg((amount < 0 ? colors().negative : colors().positive).name()));
        line->addWidget(value);
        column->addWidget(row);
    }
    if (count == 0) {
        QLabel* nothing = muted_label(QStringLiteral("Nothing yet. Payments you send and receive show up here."), rows_);
        nothing->setContentsMargins(6, 6, 6, 6);
        column->addWidget(nothing);
    }
    column->addStretch(1);
    scroll_->setWidget(rows_);
}

void OverviewPage::show_status(const ChainStatus& s, const json& mining) {
    height_->setText(QStringLiteral("#%1").arg(s.blocks));
    peers_->setText(QString::number(s.peers));
    const std::int64_t age = QDateTime::currentSecsSinceEpoch() - s.tip_time;
    if (s.headers > s.blocks) {
        sync_->setText(QStringLiteral("Catching up: %1 blocks to go").arg(s.headers - s.blocks));
    } else if (s.peers == 0) {
        sync_->setText(QStringLiteral("Looking for peers"));
    } else {
        sync_->setText(QStringLiteral("Up to date (last block %1 ago)").arg(age_text(age)));
    }
    const QString status = text_of(mining, "status");
    if (status == QLatin1String("mining")) {
        mining_->setText(QStringLiteral("On, %1").arg(hashrate_text(mining.value("hashespersec", 0.0))));
    } else if (status.isEmpty() || status == QLatin1String("off")) {
        mining_->setText(QStringLiteral("Off"));
    } else {
        mining_->setText(QStringLiteral("On, %1").arg(status));
    }
}

}  // namespace bound::gui
