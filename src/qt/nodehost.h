#pragma once

// Milestone 9: the node behind the wallet app. Like Bitcoin-Qt, the app runs a full node inside
// itself, on a thread of its own. If a node (boundd) is already running on the same data
// directory, it uses that one instead and leaves it running when the app closes.

#include "node/node.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>

namespace bound::gui {

class NodeHost : public QObject {
    Q_OBJECT

public:
    NodeHost(std::filesystem::path datadir, NodeOptions options, QObject* parent = nullptr);
    ~NodeHost() override;  // stops the node inside, and waits for it

    // Uses the node already running on the data directory, or starts one here.
    void start();
    // Asks the node inside to stop; finished() follows. Nothing for a node started elsewhere.
    void stop();

    bool embedded() const { return embedded_; }  // the node runs in this process
    bool running() const { return thread_.joinable() && !done_; }
    const std::filesystem::path& datadir() const { return datadir_; }
    std::uint16_t rpc_port() const;
    Network network() const { return options_.network; }  // milestone 10
    const ConsensusParams& params() const { return params_for(options_.network); }

signals:
    void log_line(const QString& line);  // from the node inside, timestamped
    // The node inside stopped: on request when `error` is empty, otherwise because of it.
    void finished(const QString& error);

private:
    std::filesystem::path datadir_;
    NodeOptions options_;
    bool embedded_ = false;
    std::atomic<bool> interrupt_{false};
    std::atomic<bool> done_{false};
    std::thread thread_;
};

}  // namespace bound::gui
