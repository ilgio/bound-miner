#include "qt/nodehost.h"

#include "app/datadir.h"
#include "core/params.h"
#include "rpc/client.h"
#include "util/time.h"

#include <QMetaObject>

#include <exception>
#include <string>
#include <utility>

namespace bound::gui {

NodeHost::NodeHost(std::filesystem::path datadir, NodeOptions options, QObject* parent)
    : QObject(parent), datadir_(std::move(datadir)), options_(std::move(options)) {}

NodeHost::~NodeHost() {
    interrupt_ = true;
    if (thread_.joinable()) thread_.join();
}

std::uint16_t NodeHost::rpc_port() const {
    return options_.rpc_port != 0 ? options_.rpc_port : params().default_rpc_port;
}

void NodeHost::start() {
    // A node already answering on this data directory's cookie: use it.
    try {
        RpcClient probe(cookie_path(datadir_), rpc_port(), 3000);
        probe.call("getblockcount");
        embedded_ = false;
        return;
    } catch (const std::exception&) {
        // Nobody there (or a stale cookie from a node that crashed): run one here.
    }
    embedded_ = true;
    thread_ = std::thread([this] {
        const auto post_line = [this](const std::string& text) {
            const QString line = QString::fromStdString(format_utc(unix_now()) + " " + text);
            QMetaObject::invokeMethod(this, [this, line] { emit log_line(line); }, Qt::QueuedConnection);
        };
        QString error;
        try {
            run_node(datadir_, options_, interrupt_, post_line);
        } catch (const std::exception& e) {
            error = QString::fromStdString(e.what());
            if (error.isEmpty()) error = QStringLiteral("the node stopped with an error");
            post_line(std::string("error: ") + e.what());
        }
        done_ = true;
        QMetaObject::invokeMethod(this, [this, error] { emit finished(error); }, Qt::QueuedConnection);
    });
}

void NodeHost::stop() {
    if (embedded_) interrupt_ = true;
}

}  // namespace bound::gui
