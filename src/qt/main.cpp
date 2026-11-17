// bound-qt: the BOUND desktop wallet. Like Bitcoin-Qt it's a full node with a window: it runs the
// node inside itself (or uses a boundd already running on the same data directory) and shows the
// wallet, payments, mining and the network.
//   bound-qt [--mainnet | --testnet | --regtest] [--datadir <dir>]
//            [the node options of `boundd node`: --connect, --port, --index, ...]
//   bound-qt --version
#include "app/datadir.h"
#include "node/node.h"
#include "qt/mainwindow.h"
#include "qt/nodehost.h"
#include "qt/style.h"
#include "util/version.h"

#include <QApplication>
#include <QMessageBox>
#include <QStringList>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("BOUND wallet"));
    QApplication::setOrganizationName(QStringLiteral("BOUND"));
    bound::gui::apply_style(app);
    if (bound::asks_for_version(argc, argv)) {
        const std::string text = bound::version_text("bound-qt");
#ifdef _WIN32
        // A windowed program on Windows has no console to print to.
        QMessageBox::information(nullptr, QStringLiteral("BOUND wallet"), QString::fromStdString(text));
#else
        std::fputs(text.c_str(), stdout);
#endif
        return 0;
    }

    // Qt has taken its own options (-style and so on) out already.
    std::vector<std::string> words;
    const QStringList arguments = QApplication::arguments();
    for (qsizetype i = 1; i < arguments.size(); ++i) words.push_back(arguments[i].toStdString());
    std::vector<std::string_view> views(words.begin(), words.end());
    std::optional<bound::NodeOptions> options;
    std::filesystem::path datadir;
    try {
        const bound::Network network = bound::take_network(views);  // milestone 10
        datadir = bound::default_datadir(network);
        if (views.size() >= 2 && views[0] == "--datadir") {
            datadir = std::filesystem::path(std::u8string(views[1].begin(), views[1].end()));
            views.erase(views.begin(), views.begin() + 2);
        }
        options = bound::parse_node_options(views);
        if (options) options->network = network;
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, QStringLiteral("BOUND wallet"), QString::fromStdString(e.what()));
        return 2;
    }
    if (!options) {
        QMessageBox::information(nullptr, QStringLiteral("BOUND wallet"),
                                 QStringLiteral("usage: bound-qt [--mainnet | --testnet | --regtest] [--datadir <dir>] [node options]\n\n"
                                                "The %1 unless told otherwise. The node options are those of `boundd node`: "
                                                "--connect, --seednode, --port, --rpcport, --index, --externalip, ...")
                                     .arg(QString::fromStdString(std::string(bound::network_name(bound::kDefaultNetwork)))));
        return 2;
    }

    bound::gui::NodeHost host(datadir, *options);
    host.start();
    bound::gui::MainWindow window(host);
    window.show();
    return app.exec();
}
