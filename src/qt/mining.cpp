#include "core/address.h"
#include "core/miner.h"
#include "core/params.h"
#include "qt/format.h"
#include "qt/pages.h"
#include "qt/style.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <thread>

namespace bound::gui {

MiningPage::MiningPage(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(28, 24, 28, 24);
    page->setSpacing(16);
    page->addWidget(page_title(QStringLiteral("Mining"), this));

    QFrame* control = card(this);
    auto* c = new QVBoxLayout(control);
    c->setContentsMargins(22, 20, 22, 20);
    c->setSpacing(10);
    c->addWidget(muted_label(QStringLiteral("Your node can mine new blocks with this computer's processor. Each block "
                                            "it finds pays 5 BOUND (plus the fees in it) to your address; mined coins "
                                            "can be spent after 100 more blocks."),
                             control));
    auto* row = new QHBoxLayout;
    payee_ = new QLineEdit(control);
    payee_->setFont(mono_font());
    payee_->setPlaceholderText(QStringLiteral("Pay to: a new address of your wallet (or type another)"));
    toggle_ = new QPushButton(QStringLiteral("Start mining"), control);
    toggle_->setObjectName(QStringLiteral("primary"));
    toggle_->setMinimumWidth(150);
    // Milestone 12: how many of the processor's threads mine; by default all but one.
    const int cores = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    threads_ = new QSpinBox(control);
    threads_->setRange(1, cores);
    threads_->setValue(static_cast<int>(BackgroundMiner::default_threads()));
    threads_->setSuffix(QStringLiteral(" threads"));
    threads_->setToolTip(QStringLiteral("How many of this computer's %1 processor threads mine").arg(cores));
    row->addWidget(payee_, 1);
    row->addWidget(threads_);
    row->addWidget(toggle_);
    c->addLayout(row);
    paying_ = muted_label(QString(), control);
    c->addWidget(paying_);
    page->addWidget(control);

    QFrame* stats = card(this);
    auto* g = new QGridLayout(stats);
    g->setContentsMargins(22, 18, 22, 18);
    g->setVerticalSpacing(10);
    const auto stat = [&](int r, const QString& name) {
        g->addWidget(muted_label(name, stats), r, 0);
        auto* value = new QLabel(QStringLiteral("-"), stats);
        value->setObjectName(QStringLiteral("mediumValue"));
        g->addWidget(value, r, 1, Qt::AlignRight);
        return value;
    };
    status_ = stat(0, QStringLiteral("Status"));
    rate_ = stat(1, QStringLiteral("Your hash rate"));
    found_ = stat(2, QStringLiteral("Blocks found since the node started"));
    difficulty_ = stat(3, QStringLiteral("Difficulty of the next block"));
    network_rate_ = stat(4, QStringLiteral("The whole network's hash rate (last 120 blocks)"));
    page->addWidget(stats);
    page->addStretch();

    connect(toggle_, &QPushButton::clicked, this, &MiningPage::toggle);
}

void MiningPage::show_mining(const json& info) {
    mining_ = info.value("mining", false);
    const QString status = text_of(info, "status");
    status_->setText(status.isEmpty() ? QStringLiteral("-") : status.left(1).toUpper() + status.mid(1));
    rate_->setText(hashrate_text(info.value("hashespersec", 0.0)));
    found_->setText(text_of(info, "blocksfound"));
    difficulty_->setText(QString::number(info.value("difficulty", 0.0), 'f', 2));
    toggle_->setText(mining_ ? QStringLiteral("Stop mining") : QStringLiteral("Start mining"));
    payee_->setEnabled(!mining_);
    threads_->setEnabled(!mining_);
    if (mining_) threads_->setValue(static_cast<int>(int_of(info, "threads", threads_->value())));
    paying_->setText(mining_ ? QStringLiteral("Paying %1").arg(text_of(info, "address")) : QString());
}

void MiningPage::show_network_hashrate(double hashes_per_second) { network_rate_->setText(hashrate_text(hashes_per_second)); }

void MiningPage::toggle() {
    json params = json::array({!mining_});
    if (!mining_) {
        const QString payee = payee_->text().trimmed();
        if (!payee.isEmpty()) {
            if (!decode_address(payee.toStdString(), session_.params().address_version)) {
                paying_->setText(QStringLiteral("That's not a %1 address.").arg(session_.network_name()));
                return;
            }
            params.push_back(payee.toStdString());
        } else if (!session_.has_wallet()) {
            paying_->setText(QStringLiteral("Create a wallet first (Overview), or type an address to mine to."));
            return;
        } else {
            params.push_back(nullptr);  // a new address of the wallet
        }
        params.push_back(threads_->value());
    }
    toggle_->setEnabled(false);
    session_.rpc().call(
        "setmining", params, this,
        [this](const json& info) {
            toggle_->setEnabled(true);
            show_mining(info);
            session_.refresh();
        },
        [this](const RpcFailure& f) {
            toggle_->setEnabled(true);
            Session::show_error(this, QStringLiteral("Mining"), f);
        });
}

}  // namespace bound::gui
