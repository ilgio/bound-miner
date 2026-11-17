#include "qt/format.h"
#include "qt/pages.h"
#include "qt/qr.h"
#include "qt/style.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QVBoxLayout>

#include <stdexcept>

namespace bound::gui {

namespace {

const QString kAddressKey = QStringLiteral("receive/address");
constexpr int kCodeSide = 216;  // pixels on screen

QLabel* card_title(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("cardTitle"));
    return label;
}

}  // namespace

ReceivePage::ReceivePage(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(28, 24, 28, 24);
    page->setSpacing(16);
    page->addWidget(page_title(QStringLiteral("Receive"), this));

    QFrame* box = card(this);
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(22, 20, 22, 20);
    row->setSpacing(24);

    // Milestone 18: the address as a QR code, for a phone or another screen to read.
    auto* code_box = new QFrame(box);
    code_box->setObjectName(QStringLiteral("qrBox"));
    auto* code_layout = new QVBoxLayout(code_box);
    code_layout->setContentsMargins(6, 6, 6, 6);
    code_ = new QLabel(code_box);
    code_->setFixedSize(kCodeSide, kCodeSide);
    code_->setAlignment(Qt::AlignCenter);
    code_->setStyleSheet(QStringLiteral("color: #686d84; background: white;"));
    code_layout->addWidget(code_);
    row->addWidget(code_box, 0, Qt::AlignTop);

    auto* side = new QVBoxLayout;
    side->setSpacing(10);
    side->addWidget(card_title(QStringLiteral("YOUR ADDRESS"), box));
    address_ = new QLabel(QStringLiteral("-"), box);
    address_->setFont(mono_font(13));
    address_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    address_->setWordWrap(true);
    side->addWidget(address_);
    auto* buttons = new QHBoxLayout;
    copy_ = new QPushButton(QStringLiteral("Copy address"), box);
    copy_->setObjectName(QStringLiteral("primary"));
    new_ = new QPushButton(QStringLiteral("New address"), box);
    new_->setToolTip(QStringLiteral("Another address of this wallet, for another payer"));
    buttons->addWidget(copy_);
    buttons->addWidget(new_);
    buttons->addStretch();
    side->addLayout(buttons);

    side->addSpacing(8);
    side->addWidget(card_title(QStringLiteral("ASK FOR AN AMOUNT (OPTIONAL)"), box));
    auto* ask = new QHBoxLayout;
    request_ = new QLineEdit(box);
    request_->setPlaceholderText(QStringLiteral("0.00"));
    request_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("^\\d{0,8}(\\.\\d{0,8})?$")), request_));
    request_->setMaximumWidth(180);
    copy_link_ = new QPushButton(QStringLiteral("Copy payment link"), box);
    copy_link_->setToolTip(QStringLiteral("A bound: link with the address and the amount: pasted into Send, it fills both"));
    ask->addWidget(request_);
    ask->addWidget(new QLabel(QStringLiteral("BOUND"), box));
    ask->addWidget(copy_link_);
    ask->addStretch();
    side->addLayout(ask);
    note_ = muted_label(QString(), box);
    side->addWidget(note_);
    side->addStretch();
    row->addLayout(side, 1);
    page->addWidget(box);

    page->addWidget(muted_label(QStringLiteral("<b>Why new addresses?</b> Every payment to an address is public on the chain. "
                                               "Using one address for everything lets anyone who knows it's yours see all you "
                                               "receive; a new one for each payer keeps that private. Every address the wallet "
                                               "makes stays yours: they all come from your 24 words, and the balance counts them "
                                               "all. Reusing one address works too."),
                                this));
    page->addStretch();

    connect(copy_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(shown_);
        note_->setText(QStringLiteral("Address copied."));
    });
    connect(copy_link_, &QPushButton::clicked, this, [this] {
        const std::optional<Amount> amount = parse_coins(request_->text().trimmed().toStdString());
        QApplication::clipboard()->setText(payment_link(shown_, amount && *amount > 0 ? amount : std::nullopt));
        note_->setText(QStringLiteral("Payment link copied."));
    });
    connect(request_, &QLineEdit::textChanged, this, &ReceivePage::update_code);
    connect(new_, &QPushButton::clicked, this, &ReceivePage::new_address);
    show_wallet(json());
}

void ReceivePage::show_address(const QString& address) {
    shown_ = address;
    address_->setText(address.isEmpty() ? QStringLiteral("No wallet yet: create one first (Overview).") : address);
    note_->setText(QString());
    update_code();
}

void ReceivePage::update_code() {
    const bool have = session_.valid_address(shown_);
    copy_->setEnabled(have);
    copy_link_->setEnabled(have);
    request_->setEnabled(have);
    if (!have) {
        code_->setPixmap(QPixmap());
        code_->setText(QStringLiteral("No address yet"));
        return;
    }
    // The address alone, which any QR reader shows as text; with an amount, a payment link.
    const std::optional<Amount> amount = parse_coins(request_->text().trimmed().toStdString());
    const QString text = amount && *amount > 0 ? payment_link(shown_, amount) : shown_;
    try {
        const qreal dpr = devicePixelRatioF();
        QImage image = qr_image(text, 8);
        QPixmap pixmap = QPixmap::fromImage(image.scaled(QSize(kCodeSide, kCodeSide) * dpr, Qt::KeepAspectRatio, Qt::FastTransformation));
        pixmap.setDevicePixelRatio(dpr);
        code_->setPixmap(pixmap);
    } catch (const std::length_error&) {
        code_->setPixmap(QPixmap());
        code_->setText(QStringLiteral("Too long for a QR code"));
    }
}

void ReceivePage::show_wallet(const json& info) {
    const bool have = info.is_object();
    new_->setEnabled(have);
    if (!have) {
        if (!shown_.isEmpty() || address_->text() == QLatin1String("-")) show_address(QString());
        asked_ = false;
        return;
    }
    if (asked_) return;
    asked_ = true;
    // The address shown last time, if it's still this wallet's; otherwise a new one.
    const QString saved = QSettings(session_.settings_file(), QSettings::IniFormat).value(kAddressKey).toString();
    if (saved.isEmpty()) {
        new_address();
        return;
    }
    session_.rpc().call(
        "validateaddress", json::array({saved.toStdString()}), this,
        [this, saved](const json& r) {
            if (r.value("ismine", false)) {
                show_address(saved);
            } else {
                new_address();
            }
        },
        [this](const RpcFailure&) { asked_ = false; });
}

void ReceivePage::new_address() {
    session_.rpc().call(
        "getnewaddress", json::array(), this,
        [this](const json& r) {
            const QString address = QString::fromStdString(r.get<std::string>());
            show_address(address);
            QSettings(session_.settings_file(), QSettings::IniFormat).setValue(kAddressKey, address);
        },
        [this](const RpcFailure& f) {
            asked_ = false;
            Session::show_error(this, QStringLiteral("Can't make an address"), f);
        });
}

}  // namespace bound::gui
