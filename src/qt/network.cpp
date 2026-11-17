#include "qt/format.h"
#include "qt/pages.h"
#include "qt/style.h"

#include <QGridLayout>
#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

namespace bound::gui {

NetworkPage::NetworkPage(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(28, 24, 28, 24);
    page->setSpacing(16);
    page->addWidget(page_title(QStringLiteral("Network"), this));

    QFrame* summary = card(this);
    auto* g = new QGridLayout(summary);
    g->setContentsMargins(22, 18, 22, 18);
    g->setVerticalSpacing(8);
    const auto row = [&](int r, const QString& name) {
        g->addWidget(muted_label(name, summary), r, 0);
        auto* value = new QLabel(QStringLiteral("-"), summary);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        g->addWidget(value, r, 1);
        return value;
    };
    counts_ = row(0, QStringLiteral("Connections"));
    port_ = row(1, QStringLiteral("Listening on"));
    reachable_ = row(2, QStringLiteral("Reachable from outside at"));
    version_ = row(3, QStringLiteral("Node version"));
    g->setColumnStretch(1, 1);
    page->addWidget(summary);

    peers_ = new QTableWidget(0, 5, this);
    peers_->setHorizontalHeaderLabels({QStringLiteral("Peer"), QStringLiteral("Direction"), QStringLiteral("Their height"),
                                       QStringLiteral("Software"), QStringLiteral("Blocks coming")});
    peers_->verticalHeader()->hide();
    peers_->setShowGrid(false);
    peers_->setAlternatingRowColors(true);
    peers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    peers_->setSelectionMode(QAbstractItemView::NoSelection);
    peers_->setFocusPolicy(Qt::NoFocus);
    peers_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    peers_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    page->addWidget(peers_, 1);
    page->addWidget(muted_label(QStringLiteral("Outbound peers are ones your node dialled; inbound ones dialled you, "
                                               "which only happens if others can reach this computer (see "
                                               "docs/home-node.md: port 18333)."),
                                this));
}

void NetworkPage::show_network(const json& info) {
    counts_->setText(QStringLiteral("%1 (%2 outbound, %3 inbound)")
                         .arg(text_of(info, "connections"), text_of(info, "connections_out"), text_of(info, "connections_in")));
    port_->setText(QStringLiteral("port %1").arg(text_of(info, "port")));
    QStringList addresses;
    if (const auto local = info.find("localaddresses"); local != info.end() && local->is_array()) {
        for (const json& a : *local) {
            addresses << QStringLiteral("%1:%2 (%3)")
                             .arg(text_of(a, "address"), text_of(a, "port"),
                                  text_of(a, "source") == QLatin1String("externalip") ? QStringLiteral("--externalip")
                                                                                       : QStringLiteral("as %1 peers see it").arg(text_of(a, "peers")));
        }
    }
    reachable_->setText(addresses.isEmpty() ? QStringLiteral("not known yet (no outbound peer has said)") : addresses.join(QStringLiteral(", ")));
    version_->setText(text_of(info, "subversion"));
}

void NetworkPage::show_peers(const json& list) {
    peers_->setRowCount(0);
    if (!list.is_array()) return;
    for (const json& p : list) {
        const int r = peers_->rowCount();
        peers_->insertRow(r);
        peers_->setItem(r, 0, new QTableWidgetItem(text_of(p, "addr")));
        peers_->setItem(r, 1, new QTableWidgetItem(p.value("inbound", false) ? QStringLiteral("inbound") : QStringLiteral("outbound")));
        peers_->setItem(r, 2, new QTableWidgetItem(text_of(p, "start_height")));
        peers_->setItem(r, 3, new QTableWidgetItem(text_of(p, "subver")));
        peers_->setItem(r, 4, new QTableWidgetItem(text_of(p, "blocks_in_flight")));
    }
}

}  // namespace bound::gui
