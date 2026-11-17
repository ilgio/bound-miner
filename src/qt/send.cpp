#include "core/address.h"
#include "core/params.h"
#include "core/transaction.h"
#include "qt/dialogs.h"
#include "qt/format.h"
#include "qt/icons.h"
#include "qt/pages.h"
#include "qt/style.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <string>

namespace bound::gui {

namespace {

// A payment as the app makes most of them: one input, the payee and change (191 bytes).
constexpr double kTypicalBytes = 191;

std::optional<Amount> typed_amount(const QLineEdit* field) {
    const std::optional<Amount> a = parse_coins(field->text().trimmed().toStdString());
    if (!a || *a <= 0) return std::nullopt;
    return a;
}

std::optional<double> typed_rate(const QLineEdit* field) {
    bool ok = false;
    const double v = field->text().trimmed().toDouble(&ok);
    if (!ok || v < 0 || v > 1e8) return std::nullopt;
    return v;
}

// What a typical payment pays at `units_per_byte`.
QString typical_fee(double units_per_byte) {
    return coins_text(static_cast<Amount>(std::ceil(units_per_byte * kTypicalBytes)));
}

}  // namespace

SendPage::SendPage(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(28, 24, 28, 24);
    page->setSpacing(16);
    page->addWidget(page_title(QStringLiteral("Send"), this));

    QFrame* form = card(this);
    auto* g = new QGridLayout(form);
    g->setContentsMargins(22, 20, 22, 20);
    g->setHorizontalSpacing(16);
    g->setVerticalSpacing(10);

    // Who: an address, a payment link, or a contact.
    address_ = new QLineEdit(form);
    address_->setFont(mono_font());
    {
        const QString example = QString::fromStdString(encode_address(PublicKey{}, session_.params().address_version));
        address_->setPlaceholderText(QStringLiteral("A %1 address (%2 characters starting with %3), or a bound: payment link")
                                         .arg(session_.network_name())
                                         .arg(example.size())
                                         .arg(example.left(1)));
    }
    contacts_ = new QToolButton(form);
    contacts_->setText(QStringLiteral("Contacts"));
    contacts_->setIcon(glyph_icon(Glyph::Contacts, colors().text, colors().text, 16));
    contacts_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    contacts_->setPopupMode(QToolButton::InstantPopup);
    contacts_->setMenu(new QMenu(contacts_));
    connect(contacts_->menu(), &QMenu::aboutToShow, this, &SendPage::fill_contacts_menu);
    on_theme_change(contacts_, [this] { contacts_->setIcon(glyph_icon(Glyph::Contacts, colors().text, colors().text, 16)); });
    auto* to = new QHBoxLayout;
    to->setSpacing(8);
    to->addWidget(address_, 1);
    to->addWidget(contacts_);
    address_note_ = muted_label(QString(), form);
    g->addWidget(new QLabel(QStringLiteral("Pay to"), form), 0, 0);
    g->addLayout(to, 0, 1, 1, 3);
    g->addWidget(address_note_, 1, 1, 1, 3);

    // How much, or everything.
    amount_ = new QLineEdit(form);
    amount_->setPlaceholderText(QStringLiteral("0.00"));
    amount_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("^\\d{0,8}(\\.\\d{0,8})?$")), amount_));
    amount_->setMaximumWidth(220);
    max_ = new QPushButton(QStringLiteral("Max"), form);
    max_->setCheckable(true);
    max_->setToolTip(QStringLiteral("Send everything you can spend; the fee comes out of it"));
    available_ = muted_label(QString(), form);
    available_->setWordWrap(false);
    auto* how_much = new QHBoxLayout;
    how_much->setSpacing(8);
    how_much->addWidget(amount_);
    how_much->addWidget(new QLabel(QStringLiteral("BOUND"), form));
    how_much->addWidget(max_);
    how_much->addStretch();
    g->addWidget(new QLabel(QStringLiteral("Amount"), form), 2, 0);
    g->addLayout(how_much, 2, 1, 1, 2);
    g->addWidget(available_, 2, 3, Qt::AlignRight);

    memo_ = new QLineEdit(form);
    memo_->setPlaceholderText(QStringLiteral("Optional, at most 100 bytes. Everyone can read it on the chain."));
    g->addWidget(new QLabel(QStringLiteral("Memo"), form), 3, 0);
    g->addWidget(memo_, 3, 1, 1, 3);

    // The fee, as a speed.
    slow_ = new QRadioButton(QStringLiteral("Slow"), form);
    normal_ = new QRadioButton(QStringLiteral("Normal"), form);
    fast_ = new QRadioButton(QStringLiteral("Fast"), form);
    custom_fee_ = new QRadioButton(QStringLiteral("Custom:"), form);
    slow_->setToolTip(QStringLiteral("The least the network relays, 1 unit per byte: it may wait while blocks are full"));
    normal_->setToolTip(QStringLiteral("What gets into the next block, judged by what's waiting now"));
    fast_->setToolTip(QStringLiteral("Twice the normal rate (at least 5 units per byte), ahead of the others"));
    auto* group = new QButtonGroup(this);
    for (QRadioButton* b : {slow_, normal_, fast_, custom_fee_}) group->addButton(b);
    normal_->setChecked(true);
    fee_rate_ = new QLineEdit(form);
    fee_rate_->setPlaceholderText(QStringLiteral("1"));
    fee_rate_->setMaximumWidth(90);
    fee_rate_->setEnabled(false);
    fee_note_ = muted_label(QString(), form);
    auto* speeds = new QHBoxLayout;
    speeds->setSpacing(18);
    for (QRadioButton* b : {slow_, normal_, fast_}) speeds->addWidget(b);
    speeds->addSpacing(8);
    speeds->addWidget(custom_fee_);
    speeds->addWidget(fee_rate_);
    speeds->addWidget(new QLabel(QStringLiteral("units per byte"), form));
    speeds->addStretch();
    g->addWidget(new QLabel(QStringLiteral("Fee"), form), 4, 0, Qt::AlignTop);
    g->addLayout(speeds, 4, 1, 1, 3);
    g->addWidget(fee_note_, 5, 1, 1, 3);

    send_ = new QPushButton(QStringLiteral("Send"), form);
    send_->setObjectName(QStringLiteral("primary"));
    send_->setMinimumWidth(140);
    auto* clear = new QPushButton(QStringLiteral("Clear"), form);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(clear);
    buttons->addWidget(send_);
    g->addLayout(buttons, 6, 0, 1, 4);
    g->setColumnStretch(3, 1);
    page->addWidget(form);

    page->addWidget(warning_banner(QStringLiteral("A payment can't be called back once a block has it. While it waits "
                                                  "unconfirmed, its details (Transactions) can raise its fee to speed it up."),
                                   this));
    page->addStretch();

    connect(address_, &QLineEdit::textChanged, this, [this](const QString& text) {
        // A pasted payment link fills the address and the amount.
        if (const std::optional<PaymentRequest> request = parse_payment_link(text)) {
            const QSignalBlocker block(address_);
            address_->setText(request->address);
            if (request->amount) {
                max_->setChecked(false);
                amount_->setText(amount_text(*request->amount));
            }
        }
        check();
    });
    connect(amount_, &QLineEdit::textChanged, this, &SendPage::check);
    connect(memo_, &QLineEdit::textChanged, this, &SendPage::check);
    connect(fee_rate_, &QLineEdit::textChanged, this, &SendPage::check);
    connect(custom_fee_, &QRadioButton::toggled, this, [this](bool on) {
        fee_rate_->setEnabled(on);
        check();
    });
    connect(group, &QButtonGroup::buttonToggled, this, [this] { check(); });
    connect(max_, &QPushButton::toggled, this, [this](bool on) {
        amount_->setReadOnly(on);
        if (on) {
            amount_->setText(amount_text(spendable_));
        } else {
            amount_->clear();
        }
        check();
    });
    connect(send_, &QPushButton::clicked, this, &SendPage::send);
    connect(clear, &QPushButton::clicked, this, [this] {
        address_->clear();
        max_->setChecked(false);
        amount_->clear();
        memo_->clear();
        normal_->setChecked(true);
    });
    check();
}

void SendPage::set_address(const QString& address) {
    address_->setText(address);
    if (address.isEmpty()) address_->setFocus();
    else amount_->setFocus();
}

void SendPage::show_wallet(const json& info) {
    if (!info.is_object()) {
        spendable_ = 0;
        available_->setText(QStringLiteral("No wallet"));
    } else {
        spendable_ = amount_of(info, "balance").value_or(0);
        available_->setText(QStringLiteral("Available: ") + coins_text(spendable_));
        if (max_->isChecked()) amount_->setText(amount_text(spendable_));
    }
    check();
}

void SendPage::show_fee_estimate(double units_per_byte) {
    estimate_ = units_per_byte;
    check();
}

std::optional<double> SendPage::chosen_rate() const {
    const double normal = std::max(1.0, estimate_.value_or(1.0));
    if (slow_->isChecked()) return 1.0;
    if (fast_->isChecked()) return std::max(2 * normal, 5.0);
    if (custom_fee_->isChecked()) return typed_rate(fee_rate_);
    return std::nullopt;  // Normal: the node picks what makes the next block when it builds the payment
}

void SendPage::fill_contacts_menu() {
    QMenu* menu = contacts_->menu();
    menu->clear();
    const std::vector<Contact>& list = session_.contacts().list();
    for (const Contact& c : list) {
        menu->addAction(QStringLiteral("%1   %2").arg(c.name, short_hash(c.address)), this, [this, address = c.address] { set_address(address); });
    }
    if (list.empty()) menu->addAction(QStringLiteral("No contacts yet"))->setEnabled(false);
    menu->addSeparator();
    const QString typed = address_->text().trimmed();
    QAction* save = menu->addAction(QStringLiteral("Save this address as a contact..."), this, [this, typed] { save_contact(typed); });
    save->setEnabled(session_.valid_address(typed) && !session_.contacts().name_for(typed));
}

void SendPage::save_contact(const QString& address) {
    if (const std::optional<Contact> saved = edit_contact(session_, this, Contact{QString(), address})) {
        session_.contacts().put(*saved);
        check();
        session_.refresh();
    }
}

void SendPage::check() {
    const QString address = address_->text().trimmed();
    const bool address_ok = session_.valid_address(address);
    address_note_->setVisible(!address.isEmpty());
    if (address.isEmpty()) {
        address_note_->setText(QString());
    } else if (address_ok) {
        const std::optional<QString> name = session_.contacts().name_for(address);
        address_note_->setText(name ? QStringLiteral("%1's address, from your contacts.").arg(*name)
                                    : QStringLiteral("A valid %1 address.").arg(session_.network_name()));
    } else {
        address_note_->setText(QStringLiteral("Not a %1 address: check it (the last characters are a checksum).").arg(session_.network_name()));
    }

    const bool memo_ok = memo_->text().toUtf8().size() <= static_cast<qsizetype>(Transaction::kMaxMemo);
    const std::optional<double> rate = chosen_rate();
    const bool fee_ok = !custom_fee_->isChecked() || rate.has_value();
    QString note;
    if (!memo_ok) {
        note = QStringLiteral("The memo is longer than 100 bytes.");
    } else if (custom_fee_->isChecked()) {
        note = rate ? QStringLiteral("About %1 for a typical payment.").arg(typical_fee(*rate)) : QStringLiteral("Type a fee rate.");
    } else {
        const double shown = rate.value_or(std::max(1.0, estimate_.value_or(1.0)));
        note = QStringLiteral("%1 units per byte: about %2 for a typical payment.").arg(shown, 0, 'f', shown < 10 ? 1 : 0).arg(typical_fee(shown));
    }
    if (max_->isChecked()) note += QStringLiteral(" Max sends everything you can spend, the fee taken out of it.");
    fee_note_->setText(note);
    const bool amount_ok = max_->isChecked() ? spendable_ > 0 : typed_amount(amount_).has_value();
    send_->setEnabled(session_.has_wallet() && address_ok && amount_ok && memo_ok && fee_ok);
}

void SendPage::send() {
    const QString address = address_->text().trimmed();
    const bool everything = max_->isChecked();
    const std::optional<Amount> amount = everything ? std::optional<Amount>(spendable_) : typed_amount(amount_);
    if (!amount) return;
    const std::optional<double> rate = chosen_rate();
    const QString memo = memo_->text();
    const QString speed = slow_->isChecked() ? QStringLiteral("Slow") : fast_->isChecked() ? QStringLiteral("Fast") : custom_fee_->isChecked() ? QStringLiteral("Custom") : QStringLiteral("Normal");
    const double shown_rate = rate.value_or(std::max(1.0, estimate_.value_or(1.0)));
    const QString what = everything ? QStringLiteral("everything you can spend, %1 less the fee,").arg(coins_text(*amount)) : coins_text(*amount);
    if (QMessageBox::question(this, QStringLiteral("Send BOUND"),
                              QStringLiteral("Send %1 to\n%2?\n\nFee: %3, %4 units per byte (about %5)")
                                  .arg(what, session_.who(address), speed)
                                  .arg(shown_rate, 0, 'f', 1)
                                  .arg(typical_fee(shown_rate))) != QMessageBox::Yes) {
        return;
    }
    json params = json::array({address.toStdString()});
    if (!everything) params.push_back(format_coins(*amount));
    if (rate || !memo.isEmpty()) params.push_back(rate ? json(*rate) : json(nullptr));
    if (!memo.isEmpty()) params.push_back(memo.toStdString());
    const std::string method = everything ? "sendall" : "sendtoaddress";

    QPointer<SendPage> self(this);
    session_.with_unlocked(this, QStringLiteral("send the payment"), [this, self, params, method, address](Session::Finish finish) {
        session_.rpc().call(
            method, params, this,
            [this, finish, address](const json& result) {
                finish();
                const QString txid = result.is_string() ? QString::fromStdString(result.get<std::string>()) : text_of(result, "txid");
                const QString sent = result.is_object() ? QStringLiteral("%1 BOUND went, with a fee of %2 BOUND.\n").arg(text_of(result, "amount"), text_of(result, "fee"))
                                                        : QString();
                max_->setChecked(false);
                address_->clear();
                amount_->clear();
                memo_->clear();
                QMessageBox box(QMessageBox::Information, QStringLiteral("Sent"),
                                QStringLiteral("The payment is on its way.\n%1Transaction ID: %2").arg(sent, txid), QMessageBox::Ok, this);
                QPushButton* save = nullptr;
                if (!session_.contacts().name_for(address)) save = box.addButton(QStringLiteral("Save as a contact..."), QMessageBox::ActionRole);
                box.exec();
                if (save != nullptr && box.clickedButton() == save) save_contact(address);
                session_.refresh();
            },
            [self, finish](const RpcFailure& f) {
                finish();
                Session::show_error(self, QStringLiteral("Can't send"), f);
            });
    });
}

}  // namespace bound::gui
