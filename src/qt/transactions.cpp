#include "qt/dialogs.h"
#include "qt/format.h"
#include "qt/icons.h"
#include "qt/pages.h"
#include "qt/style.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QTableWidget>
#include <QVBoxLayout>

namespace bound::gui {

namespace {

constexpr int kTxidRole = Qt::UserRole;
constexpr int kSortRole = Qt::UserRole + 1;

// An item sorted by a number of its own (kSortRole) rather than by its text.
class SortedItem : public QTableWidgetItem {
public:
    SortedItem(const QString& text, double key) : QTableWidgetItem(text) { setData(kSortRole, key); }
    bool operator<(const QTableWidgetItem& other) const override {
        return data(kSortRole).toDouble() < other.data(kSortRole).toDouble();
    }
};

bool passes(const QString& category, int filter) {
    switch (filter) {
    case 1: return category == QLatin1String("receive");
    case 2: return category == QLatin1String("send");
    case 3: return category == QLatin1String("generate") || category == QLatin1String("immature") || category == QLatin1String("orphan");
    default: return true;
    }
}

Glyph glyph_for(const QString& category) {
    if (category == QLatin1String("send")) return Glyph::Sent;
    if (category == QLatin1String("receive")) return Glyph::Received;
    return Glyph::Mined;
}

}  // namespace

TransactionsPage::TransactionsPage(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(28, 24, 28, 24);
    page->setSpacing(14);
    auto* top = new QHBoxLayout;
    top->setSpacing(10);
    top->addWidget(page_title(QStringLiteral("Transactions"), this));
    top->addStretch();
    // Milestone 18: search by name, address, ID, amount or date.
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Search: name, address, ID, amount"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(260);
    top->addWidget(search_);
    filter_ = new QComboBox(this);
    filter_->addItems({QStringLiteral("All"), QStringLiteral("Received"), QStringLiteral("Sent"), QStringLiteral("Mined")});
    top->addWidget(filter_);
    page->addLayout(top);

    table_ = new QTableWidget(0, 5, this);
    table_->setHorizontalHeaderLabels(
        {QStringLiteral("Date"), QStringLiteral("Type"), QStringLiteral("Address"), QStringLiteral("Amount"), QStringLiteral("Confirmations")});
    table_->verticalHeader()->hide();
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setIconSize(QSize(16, 16));
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->horizontalHeader()->setSortIndicator(0, Qt::DescendingOrder);  // newest first
    table_->setSortingEnabled(true);
    page->addWidget(table_, 1);
    empty_ = muted_label(QStringLiteral("No transactions yet."), this);
    page->addWidget(empty_);
    page->addWidget(muted_label(QStringLiteral("Double-click a transaction (or press Enter) for its details and its page on the explorer."), this));

    connect(filter_, &QComboBox::currentIndexChanged, this, &TransactionsPage::fill);
    connect(search_, &QLineEdit::textChanged, this, &TransactionsPage::fill);
    connect(table_, &QTableWidget::activated, this, [this](const QModelIndex& index) {
        if (const QTableWidgetItem* item = table_->item(index.row(), 0)) show_transaction(session_, this, item->data(kTxidRole).toString());
    });
}

void TransactionsPage::show_transactions(const json& list) {
    if (list == list_ && contacts_version_ == session_.contacts().version()) return;
    list_ = list.is_array() ? list : json::array();
    fill();
}

void TransactionsPage::fill() {
    contacts_version_ = session_.contacts().version();
    const QString query = search_->text().trimmed();
    const int sort_column = table_->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder order = table_->horizontalHeader()->sortIndicatorOrder();
    table_->setSortingEnabled(false);
    table_->setRowCount(0);
    for (auto it = list_.rbegin(); it != list_.rend(); ++it) {
        const json& tx = *it;
        const QString category = text_of(tx, "category");
        if (!passes(category, filter_->currentIndex())) continue;
        const std::int64_t time = int_of(tx, "time");
        const QString txid = text_of(tx, "txid");
        const QString address = text_of(tx, "address");
        const std::optional<QString> name = session_.contacts().name_for(address);
        const Amount amount = amount_of(tx, "amount").value_or(0);
        const QString shown_address = name ? QStringLiteral("%1  (%2)").arg(*name, short_hash(address)) : address;
        if (!query.isEmpty()) {
            const QString haystack = QStringList{date_text(time), category_text(category), address, name.value_or(QString()),
                                                 amount_text(amount), txid}
                                         .join(QLatin1Char('\n'));
            if (!haystack.contains(query, Qt::CaseInsensitive)) continue;
        }
        const int row = table_->rowCount();
        table_->insertRow(row);
        auto* date = new SortedItem(date_text(time), static_cast<double>(time));
        date->setData(kTxidRole, txid);
        table_->setItem(row, 0, date);
        const QColor tone = category == QLatin1String("send") ? colors().negative
                            : category == QLatin1String("receive") ? colors().positive
                                                                   : colors().accent;
        auto* type = new QTableWidgetItem(QIcon(glyph_pixmap(glyph_for(category), tone, 16)), category_text(category));
        table_->setItem(row, 1, type);
        auto* who = new QTableWidgetItem(shown_address);
        if (!name) who->setFont(mono_font());
        who->setToolTip(address);
        table_->setItem(row, 2, who);
        auto* value = new SortedItem(coins_text(amount, true), static_cast<double>(amount));
        value->setForeground(amount < 0 ? colors().negative : colors().positive);
        value->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, 3, value);
        const std::int64_t confirmations = int_of(tx, "confirmations");
        auto* conf = new SortedItem(confirmations < 0    ? QStringLiteral("lost")
                                    : confirmations == 0 ? QStringLiteral("unconfirmed")
                                                         : QString::number(confirmations),
                                    static_cast<double>(confirmations));
        conf->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, 4, conf);
    }
    table_->setSortingEnabled(true);
    table_->sortByColumn(sort_column, order);
    empty_->setText(list_.empty() ? QStringLiteral("No transactions yet.") : QStringLiteral("Nothing matches."));
    empty_->setVisible(table_->rowCount() == 0);
}

}  // namespace bound::gui
