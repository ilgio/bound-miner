#include "qt/dialogs.h"

#include "core/params.h"
#include "qt/format.h"
#include "qt/session.h"
#include "qt/style.h"
#include "util/version.h"
#include "wallet/mnemonic.h"

#include <sodium.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace bound::gui {

namespace {

constexpr int kMinPassphrase = 8;

QLineEdit* password_field(QWidget* parent) {
    auto* field = new QLineEdit(parent);
    field->setEchoMode(QLineEdit::Password);
    field->setMinimumWidth(320);
    return field;
}

// A busy indicator while something slow (a rescan) runs; closed by deleting it.
QProgressDialog* busy(QWidget* parent, const QString& text) {
    auto* dialog = new QProgressDialog(text, QString(), 0, 0, parent);
    dialog->setWindowTitle(QStringLiteral("Please wait"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setCancelButton(nullptr);
    dialog->setMinimumDuration(0);
    dialog->show();
    return dialog;
}

// Two passphrase fields that must match, at least kMinPassphrase long; "" if cancelled. With
// `old`, a field for the current passphrase too, whose text goes there.
QString new_passphrase(QWidget* parent, const QString& title, const QString& explanation, QString* old = nullptr) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(muted_label(explanation, &dialog));
    auto* form = new QFormLayout;
    QLineEdit* old_field = nullptr;
    if (old != nullptr) {
        old_field = password_field(&dialog);
        form->addRow(QStringLiteral("Current passphrase"), old_field);
    }
    QLineEdit* first = password_field(&dialog);
    QLineEdit* second = password_field(&dialog);
    form->addRow(QStringLiteral("New passphrase"), first);
    form->addRow(QStringLiteral("Repeat it"), second);
    layout->addLayout(form);
    auto* problem = new QLabel(&dialog);
    problem->setStyleSheet(QStringLiteral("color: %1;").arg(colors().negative.name()));
    layout->addWidget(problem);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setObjectName(QStringLiteral("primary"));
    const auto check = [&] {
        QString why;
        if (old_field != nullptr && old_field->text().isEmpty()) {
            why = QStringLiteral("Type the current passphrase.");
        } else if (first->text().size() < kMinPassphrase) {
            why = QStringLiteral("At least %1 characters.").arg(kMinPassphrase);
        } else if (first->text() != second->text()) {
            why = QStringLiteral("The two don't match.");
        }
        problem->setText(why);
        ok->setEnabled(why.isEmpty());
    };
    QObject::connect(first, &QLineEdit::textChanged, &dialog, check);
    QObject::connect(second, &QLineEdit::textChanged, &dialog, check);
    if (old_field != nullptr) QObject::connect(old_field, &QLineEdit::textChanged, &dialog, check);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    check();
    if (dialog.exec() != QDialog::Accepted) return {};
    if (old != nullptr) *old = old_field->text();
    return first->text();
}

// The seed's dialog. A fresh seed's can't be closed (Esc, the title bar's X) before the box is ticked.
class SeedDialog : public QDialog {
public:
    using QDialog::QDialog;
    QCheckBox* must_tick = nullptr;

    void reject() override {
        if (must_tick == nullptr || must_tick->isChecked()) QDialog::reject();
    }
};

// Milestone 17: the 24 words, numbered, down three columns (1-8, 9-16, 17-24).
QString words_table(const QString& words) {
    const QStringList list = words.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    constexpr int kColumns = 3;
    const int rows = (static_cast<int>(list.size()) + kColumns - 1) / kColumns;
    QString html = QStringLiteral("<table cellspacing='0' cellpadding='3'>");
    for (int r = 0; r < rows; ++r) {
        html += QStringLiteral("<tr>");
        for (int c = 0; c < kColumns; ++c) {
            const int i = c * rows + r;
            if (i >= list.size()) continue;
            html += QStringLiteral("<td align='right' style='color: %1;'>%2.</td><td style='padding-right: 22px;'><b>%3</b></td>")
                        .arg(colors().muted.name())
                        .arg(i + 1)
                        .arg(list[i].toHtmlEscaped());
        }
        html += QStringLiteral("</tr>");
    }
    return html + QStringLiteral("</table>");
}

// While someone types the words, the last one may be unfinished: the beginning of a word on the
// list isn't a mistake yet. "" when the text is a whole seed.
QString seed_status(const QString& text) {
    std::string typed = text.toStdString();
    std::string problem;
    std::optional<Seed> seed = parse_seed(typed, &problem);
    sodium_memzero(typed.data(), typed.size());
    if (seed) {
        sodium_memzero(seed->data(), seed->size());
        return {};
    }
    const QStringList words = text.toLower().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (!words.isEmpty() && !text.back().isSpace() && words.size() <= static_cast<qsizetype>(kSeedWords)) {
        const std::string last = words.back().toStdString();
        const bool earlier_ok =
            std::all_of(words.begin(), words.end() - 1, [](const QString& w) { return word_index(w.toStdString()).has_value(); });
        const auto next = std::lower_bound(kWordList.begin(), kWordList.end(), std::string_view(last));
        const bool beginning = next != kWordList.end() && next->starts_with(last);
        if (earlier_ok && beginning && !word_index(last)) {
            return QStringLiteral("%1 of %2 words.").arg(words.size() - 1).arg(kSeedWords);
        }
    }
    return QString::fromStdString(problem);
}

}  // namespace

std::optional<QString> ask_passphrase(QWidget* parent, const QString& purpose) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Unlock the wallet"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Your passphrase, to %1:").arg(purpose), &dialog));
    QLineEdit* field = password_field(&dialog);
    layout->addWidget(field);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primary"));
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Unlock"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || field->text().isEmpty()) return std::nullopt;
    return field->text();
}

void offer_wallet(Session& session, QWidget* parent) {
    QMessageBox box(parent);
    box.setWindowTitle(QStringLiteral("Welcome to BOUND"));
    box.setIconPixmap(logo(64));
    box.setText(QStringLiteral("<b>You don't have a wallet yet.</b>"));
    const ConsensusParams& p = session.params();
    QString network_note;
    if (p.network_name != "mainnet") {
        network_note = QStringLiteral("\n\nThis is the %1: its coins have no value, so it's the place to try things.").arg(session.network_name());
    } else if (!p.launched) {
        network_note = QStringLiteral("\n\nMainnet hasn't launched: this chain is a preview, and its coins will be thrown away.");
    } else {
        network_note = QStringLiteral("\n\nThis is mainnet: keep the 24 words safe, they're the only way back to your coins.");
    }
    box.setInformativeText(QStringLiteral("A wallet holds the keys to your coins. It lives in this app's data folder, encrypted "
                                          "with a passphrase, and all of its keys come from 24 words you write down.") +
                           network_note);
    QPushButton* create = box.addButton(QStringLiteral("Create a new wallet"), QMessageBox::AcceptRole);
    create->setObjectName(QStringLiteral("primary"));
    QPushButton* restore = box.addButton(QStringLiteral("Restore from 24 words"), QMessageBox::ActionRole);
    box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == create) create_wallet(session, parent);
    if (box.clickedButton() == restore) restore_wallet(session, parent);
}

void create_wallet(Session& session, QWidget* parent) {
    QString passphrase = new_passphrase(
        parent, QStringLiteral("Create a wallet"),
        QStringLiteral("Choose a passphrase. It encrypts the wallet on this computer and is asked for whenever you "
                       "spend. If you forget it, only the wallet's 24 words (shown next) can bring the coins back."));
    if (passphrase.isEmpty()) return;
    QPointer<QWidget> owner(parent);
    session.rpc().call(
        "createwallet", json::array({passphrase.toStdString()}), &session,
        [&session, owner](const json& result) {
            show_seed(owner, text_of(result, "words"), true);
            session.refresh();
        },
        [owner](const RpcFailure& f) { Session::show_error(owner, QStringLiteral("Can't create the wallet"), f); });
    passphrase.fill(QLatin1Char(' '));
}

void restore_wallet(Session& session, QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Restore a wallet"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(muted_label(QStringLiteral("Type the wallet's 24 words in order, with spaces or line breaks between "
                                                 "them. A backup from before version 1.0 is 64 hexadecimal digits instead: "
                                                 "type those. The node then scans the whole chain for the wallet's coins."),
                                  &dialog));
    auto* seed_field = new QPlainTextEdit(&dialog);
    seed_field->setFont(mono_font());
    seed_field->setFixedHeight(90);
    seed_field->setMinimumWidth(460);
    layout->addWidget(seed_field);
    auto* problem = new QLabel(&dialog);
    problem->setStyleSheet(QStringLiteral("color: %1;").arg(colors().negative.name()));
    layout->addWidget(problem);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("primary"));
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Next"));
    layout->addWidget(buttons);
    const auto check = [&] {
        const QString text = seed_field->toPlainText();
        const QString why = text.trimmed().isEmpty() ? QStringLiteral("0 of %1 words.").arg(kSeedWords) : seed_status(text);
        problem->setText(why);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(why.isEmpty());
    };
    QObject::connect(seed_field, &QPlainTextEdit::textChanged, &dialog, check);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    check();
    if (dialog.exec() != QDialog::Accepted) return;
    QString seed = seed_field->toPlainText().simplified();  // the words, one space apart (hex: spaces don't matter)
    seed_field->clear();

    QString passphrase = new_passphrase(parent, QStringLiteral("Restore a wallet"),
                                        QStringLiteral("Choose a passphrase to encrypt the restored wallet on this computer."));
    if (passphrase.isEmpty()) return;
    QPointer<QWidget> owner(parent);
    QPointer<QProgressDialog> wait = busy(parent, QStringLiteral("Scanning the chain for the wallet's coins..."));
    session.rpc().call(
        "restorewallet", json::array({seed.toStdString(), passphrase.toStdString()}), &session,
        [&session, owner, wait](const json& result) {
            delete wait.data();
            QMessageBox::information(owner, QStringLiteral("Wallet restored"),
                                     QStringLiteral("Found %1 transactions. Spendable now: %2 BOUND.")
                                         .arg(text_of(result, "transactions"), text_of(result, "balance")));
            session.refresh();
        },
        [owner, wait](const RpcFailure& f) {
            delete wait.data();
            Session::show_error(owner, QStringLiteral("Can't restore the wallet"), f);
        });
    seed.fill(QLatin1Char(' '));
    passphrase.fill(QLatin1Char(' '));
}

void show_seed(QWidget* parent, const QString& seed, bool fresh) {
    SeedDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Your wallet's 24 words"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        fresh ? QStringLiteral("<b>Write these 24 words down</b>, in order, and keep them somewhere safe, offline.")
              : QStringLiteral("<b>Your wallet's 24 words.</b> Keep them somewhere safe, offline."),
        &dialog));
    layout->addWidget(muted_label(QStringLiteral("Every key of the wallet comes from them: with the words, the wallet can be "
                                                 "restored on any computer. Anyone who sees them can take your coins, so "
                                                 "never type them into a website or send them to anyone. Only BOUND's "
                                                 "wallet restores BOUND coins from them: other wallets would open an empty "
                                                 "wallet of their own."),
                                  &dialog));
    auto* text = new QLabel(words_table(seed), &dialog);
    text->setTextFormat(Qt::RichText);
    text->setFont(mono_font(13));
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text->setAlignment(Qt::AlignCenter);
    text->setObjectName(QStringLiteral("banner"));
    layout->addWidget(text);
    auto* written = new QCheckBox(QStringLiteral("I've written it down"), &dialog);
    if (fresh) layout->addWidget(written);
    auto* buttons = new QDialogButtonBox(&dialog);
    QPushButton* copy = buttons->addButton(QStringLiteral("Copy"), QDialogButtonBox::ActionRole);
    QPushButton* done = buttons->addButton(QStringLiteral("Done"), QDialogButtonBox::AcceptRole);
    done->setObjectName(QStringLiteral("primary"));
    done->setEnabled(!fresh);
    layout->addWidget(buttons);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [seed] { QApplication::clipboard()->setText(seed); });
    QObject::connect(written, &QCheckBox::toggled, done, &QPushButton::setEnabled);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (fresh) dialog.must_tick = written;
    dialog.exec();
    if (QApplication::clipboard()->text() == seed) QApplication::clipboard()->clear();
}

void backup_seed(Session& session, QWidget* parent) {
    if (!session.has_wallet()) return;
    QPointer<QWidget> owner(parent);
    session.with_unlocked(parent, QStringLiteral("show the seed"), [&session, owner](Session::Finish finish) {
        session.rpc().call(
            "dumpseed", json::array(), &session,
            [owner, finish](const json& seed) {
                show_seed(owner, text_of(seed, "words"), false);
                finish();
            },
            [owner, finish](const RpcFailure& f) {
                Session::show_error(owner, QStringLiteral("Can't show the seed"), f);
                finish();
            });
    });
}

void change_passphrase(Session& session, QWidget* parent) {
    if (!session.has_wallet()) return;
    QString old;
    QString next = new_passphrase(parent, QStringLiteral("Change the passphrase"),
                                  QStringLiteral("The wallet is encrypted again under the new passphrase. The seed doesn't change."),
                                  &old);
    if (next.isEmpty()) return;
    QPointer<QWidget> owner(parent);
    session.rpc().call(
        "walletpassphrasechange", json::array({old.toStdString(), next.toStdString()}), &session,
        [owner](const json&) { QMessageBox::information(owner, QStringLiteral("Passphrase changed"), QStringLiteral("Done.")); },
        [owner](const RpcFailure& f) { Session::show_error(owner, QStringLiteral("Can't change the passphrase"), f); });
    old.fill(QLatin1Char(' '));
    next.fill(QLatin1Char(' '));
}

void show_transaction(Session& session, QWidget* parent, const QString& txid) {
    QPointer<QWidget> owner(parent);
    session.rpc().call(
        "gettransaction", json::array({txid.toStdString()}), &session,
        [&session, owner, txid](const json& tx) {
            QDialog dialog(owner);
            dialog.setWindowTitle(QStringLiteral("Transaction"));
            dialog.setMinimumWidth(620);
            auto* layout = new QVBoxLayout(&dialog);
            auto* form = new QFormLayout;
            form->setHorizontalSpacing(18);
            const std::int64_t confirmations = int_of(tx, "confirmations");
            const QString category = text_of(tx, "category");
            const QString status = confirmations < 0    ? QStringLiteral("Lost: its block left the chain in a reorganisation")
                                   : confirmations == 0 ? QStringLiteral("Unconfirmed: waiting in the mempool")
                                                        : QStringLiteral("%1 confirmations").arg(confirmations);
            const auto add = [&](const QString& name, const QString& value, bool mono = false) {
                auto* label = new QLabel(value, &dialog);
                label->setTextInteractionFlags(Qt::TextSelectableByMouse);
                label->setWordWrap(true);
                if (mono) label->setFont(mono_font());
                form->addRow(name, label);
            };
            add(QStringLiteral("Status"), status);
            add(QStringLiteral("Date"), date_text(int_of(tx, "time")));
            add(QStringLiteral("Type"), category_text(category));
            const std::optional<Amount> amount = amount_of(tx, "amount");
            add(QStringLiteral("Amount"), amount ? coins_text(*amount, true) : text_of(tx, "amount"));
            if (const std::optional<Amount> fee = amount_of(tx, "fee")) add(QStringLiteral("Fee"), coins_text(*fee));
            if (tx.contains("address")) {
                const QString address = text_of(tx, "address");
                if (const std::optional<QString> name = session.contacts().name_for(address)) add(QStringLiteral("Contact"), *name);
                add(QStringLiteral("Address"), address, true);
            }
            if (const QString memo = text_of(tx, "memo_text"); !memo.isEmpty()) add(QStringLiteral("Memo"), memo);
            add(QStringLiteral("Transaction ID"), text_of(tx, "txid"), true);
            if (tx.contains("blockheight")) {
                add(QStringLiteral("Block"), QStringLiteral("#%1  %2").arg(text_of(tx, "blockheight"), short_hash(text_of(tx, "blockhash"))));
            }
            QString outputs;
            if (const auto details = tx.find("details"); details != tx.end() && details->is_array()) {
                for (const json& out : *details) {
                    outputs += QStringLiteral("%1  %2%3\n")
                                   .arg(text_of(out, "address"), text_of(out, "amount"),
                                        out.value("mine", false) ? QStringLiteral("  (yours)") : QString());
                }
            }
            add(QStringLiteral("Outputs"), outputs.trimmed(), true);
            layout->addLayout(form);

            auto* buttons = new QDialogButtonBox(&dialog);
            QPushButton* copy = buttons->addButton(QStringLiteral("Copy ID"), QDialogButtonBox::ActionRole);
            QObject::connect(copy, &QPushButton::clicked, &dialog, [txid] { QApplication::clipboard()->setText(txid); });
            // Milestone 18: the same transaction on the public explorer, as anyone else sees it.
            const std::string network = session.network_name().toStdString();
            if (const std::optional<QString> url = explorer_tx_url(network, txid)) {
                QPushButton* explorer = buttons->addButton(QStringLiteral("Open in explorer"), QDialogButtonBox::ActionRole);
                explorer->setToolTip(*url);
                QObject::connect(explorer, &QPushButton::clicked, &dialog, [link = *url] { QDesktopServices::openUrl(QUrl(link)); });
            }
            if (category == QLatin1String("send") && confirmations == 0) {
                QPushButton* bump = buttons->addButton(QStringLiteral("Raise the fee"), QDialogButtonBox::ActionRole);
                bump->setToolTip(QStringLiteral("Replace it with the same payment paying a higher fee (replace-by-fee)"));
                QObject::connect(bump, &QPushButton::clicked, &dialog, [&session, &dialog, txid] {
                    QPointer<QWidget> top(dialog.parentWidget());
                    dialog.accept();
                    session.with_unlocked(top, QStringLiteral("raise the fee"), [&session, top, txid](Session::Finish finish) {
                        session.rpc().call(
                            "bumpfee", json::array({txid.toStdString()}), &session,
                            [&session, top, finish](const json& r) {
                                finish();
                                QMessageBox::information(top, QStringLiteral("Fee raised"),
                                                         QStringLiteral("The replacement pays %1 BOUND (was %2).\nIts ID: %3")
                                                             .arg(text_of(r, "fee"), text_of(r, "origfee"), text_of(r, "txid")));
                                session.refresh();
                            },
                            [top, finish](const RpcFailure& f) {
                                finish();
                                Session::show_error(top, QStringLiteral("Can't raise the fee"), f);
                            });
                    });
                });
            }
            QPushButton* close = buttons->addButton(QDialogButtonBox::Close);
            close->setObjectName(QStringLiteral("primary"));
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            dialog.exec();
        },
        [owner](const RpcFailure& f) { Session::show_error(owner, QStringLiteral("Can't show the transaction"), f); });
}

std::optional<Contact> edit_contact(const Session& session, QWidget* parent, const Contact& contact) {
    QDialog dialog(parent);
    const bool fresh = contact.name.isEmpty();
    dialog.setWindowTitle(fresh ? QStringLiteral("New contact") : QStringLiteral("Edit a contact"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(contact.name, &dialog);
    name->setMaxLength(60);
    name->setPlaceholderText(QStringLiteral("Who it is, as you'll look for them"));
    auto* address = new QLineEdit(contact.address, &dialog);
    address->setFont(mono_font());
    address->setMinimumWidth(460);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Address"), address);
    layout->addLayout(form);
    auto* problem = new QLabel(&dialog);
    problem->setStyleSheet(QStringLiteral("color: %1;").arg(colors().negative.name()));
    layout->addWidget(problem);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QPushButton* save = buttons->button(QDialogButtonBox::Save);
    save->setObjectName(QStringLiteral("primary"));
    layout->addWidget(buttons);
    const auto check = [&] {
        const QString typed = address->text().trimmed();
        const std::optional<QString> taken = session.contacts().name_for(typed);
        QString why;
        if (name->text().trimmed().isEmpty()) {
            why = QStringLiteral("Give it a name.");
        } else if (!session.valid_address(typed)) {
            why = QStringLiteral("Not a %1 address.").arg(session.network_name());
        } else if (taken && typed != contact.address) {
            why = QStringLiteral("That address is saved already, as %1.").arg(*taken);
        }
        problem->setText(why);
        save->setEnabled(why.isEmpty());
    };
    QObject::connect(name, &QLineEdit::textChanged, &dialog, check);
    QObject::connect(address, &QLineEdit::textChanged, &dialog, check);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    check();
    name->setFocus();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return Contact{name->text().trimmed(), address->text().trimmed()};
}

void show_about(QWidget* parent) {
    QMessageBox box(parent);
    box.setWindowTitle(QStringLiteral("About BOUND"));
    box.setIconPixmap(logo(72));
    box.setText(QStringLiteral("<h3>BOUND wallet %1</h3>").arg(QString::fromStdString(std::string(version()))));
    box.setInformativeText(
        QStringLiteral("<p>A proof-of-work cryptocurrency written from scratch in C++20: UTXO ledger, BoundHash mining (its own "
                       "memory-hard proof of work), "
                       "Ed25519 signatures. This app is a full node with a wallet, like Bitcoin-Qt.</p>"
                       "<p>Networks: mainnet%3, testnet and regtest, whose coins have no value.</p>"
                       "<p>MIT License. Built with Qt %1%2.<br>"
                       "<a href=\"https://github.com/Sonic-Bound/Bound\">github.com/Sonic-Bound/Bound</a></p>")
            .arg(QString::fromLatin1(qVersion()),
                 build_commit().empty() ? QString() : QStringLiteral(" from commit %1").arg(QString::fromStdString(std::string(build_commit()))),
                 mainnet_params().launched ? QString() : QStringLiteral(" (from launch day)")));
    box.setTextFormat(Qt::RichText);
    box.exec();
}

}  // namespace bound::gui
