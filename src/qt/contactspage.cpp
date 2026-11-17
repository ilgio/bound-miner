// Milestone 18: the Contacts page, names for the addresses you pay.
#include "qt/dialogs.h"
#include "qt/format.h"
#include "qt/pages.h"
#include "qt/style.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace bound::gui {

ContactsPage::ContactsPage(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(28, 24, 28, 24);
    page->setSpacing(14);
    auto* top = new QHBoxLayout;
    top->addWidget(page_title(QStringLiteral("Contacts"), this));
    top->addStretch();
    auto* add = new QPushButton(QStringLiteral("Add a contact"), this);
    add->setObjectName(QStringLiteral("primary"));
    top->addWidget(add);
    page->addLayout(top);

    table_ = new QTableWidget(0, 2, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Address")});
    table_->verticalHeader()->hide();
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setHighlightSections(false);
    page->addWidget(table_, 1);
    empty_ = muted_label(QStringLiteral("No contacts yet. Save the people you pay, then pick them on Send instead of pasting "
                                        "their address. After a payment, Send offers to save the address too."),
                         this);
    page->addWidget(empty_);

    auto* actions = new QHBoxLayout;
    pay_ = new QPushButton(QStringLiteral("Pay"), this);
    edit_ = new QPushButton(QStringLiteral("Edit"), this);
    delete_ = new QPushButton(QStringLiteral("Delete"), this);
    actions->addWidget(pay_);
    actions->addWidget(edit_);
    actions->addWidget(delete_);
    actions->addStretch();
    page->addLayout(actions);
    page->addWidget(muted_label(QStringLiteral("Contacts are kept by this app on this computer, apart from the wallet: the 24 "
                                               "words don't bring them back."),
                                this));

    connect(add, &QPushButton::clicked, this, [this] { edit(std::nullopt); });
    connect(edit_, &QPushButton::clicked, this, [this] { edit(selected()); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this] { edit(selected()); });
    connect(pay_, &QPushButton::clicked, this, [this] {
        if (const std::optional<Contact> c = selected()) session_.pay(c->address);
    });
    connect(delete_, &QPushButton::clicked, this, [this] {
        const std::optional<Contact> c = selected();
        if (!c) return;
        if (QMessageBox::question(this, QStringLiteral("Delete a contact"),
                                  QStringLiteral("Delete %1?\nPayments to the address stay as they are.").arg(c->name)) != QMessageBox::Yes) {
            return;
        }
        session_.contacts().remove(c->address);
        fill();
        session_.refresh();
    });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        const bool any = selected().has_value();
        pay_->setEnabled(any);
        edit_->setEnabled(any);
        delete_->setEnabled(any);
    });
    fill();
}

std::optional<Contact> ContactsPage::selected() const {
    const QList<QTableWidgetItem*> items = table_->selectedItems();
    if (items.isEmpty()) return std::nullopt;
    const int row = items.front()->row();
    const QTableWidgetItem* name = table_->item(row, 0);
    const QTableWidgetItem* address = table_->item(row, 1);
    if (name == nullptr || address == nullptr) return std::nullopt;
    return Contact{name->text(), address->text()};
}

void ContactsPage::edit(std::optional<Contact> contact) {
    const QString old_address = contact ? contact->address : QString();
    if (const std::optional<Contact> saved = edit_contact(session_, this, contact.value_or(Contact{}))) {
        if (!old_address.isEmpty() && old_address != saved->address) session_.contacts().remove(old_address);
        session_.contacts().put(*saved);
        fill();
        session_.refresh();
    }
}

void ContactsPage::fill() {
    table_->setRowCount(0);
    for (const Contact& c : session_.contacts().list()) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(c.name));
        auto* address = new QTableWidgetItem(c.address);
        address->setFont(mono_font());
        table_->setItem(row, 1, address);
    }
    empty_->setVisible(table_->rowCount() == 0);
    pay_->setEnabled(false);
    edit_->setEnabled(false);
    delete_->setEnabled(false);
}

}  // namespace bound::gui
