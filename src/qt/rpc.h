#pragma once

// Milestone 9: the wallet app talks to its node the way bound-cli does, over JSON-RPC on
// 127.0.0.1, so it works the same with the node inside it or with a boundd started elsewhere.
// Calls go out on a thread of their own and their answers come back on the GUI thread.

#include "rpc/client.h"
#include "rpc/server.h"

#include <QObject>
#include <QString>
#include <QThread>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace bound::gui {

struct RpcFailure {
    int code = 0;              // the node's error code; 0 when it couldn't be reached
    QString message;
    bool unreachable = false;  // no node answering (yet), rather than an error from it
};

class Rpc : public QObject {
public:
    Rpc(std::filesystem::path cookie_file, std::uint16_t port, QObject* parent = nullptr);
    ~Rpc() override;  // waits for a call in progress

    using Done = std::function<void(const json&)>;
    using Failed = std::function<void(const RpcFailure&)>;

    // Calls `method` in the background. `done` or `failed` runs on the GUI thread afterwards, unless
    // `context` was destroyed meanwhile. Calls are answered in the order they're made.
    void call(const std::string& method, json params, QObject* context, Done done, Failed failed = {});

private:
    QThread thread_;
    QObject* worker_;                     // lives on thread_
    std::unique_ptr<RpcClient> client_;   // used on thread_ only
};

}  // namespace bound::gui
