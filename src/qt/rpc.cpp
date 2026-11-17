#include "qt/rpc.h"

#include "rpc/http.h"

#include <QMetaObject>
#include <QPointer>

#include <exception>
#include <optional>
#include <utility>

namespace bound::gui {

Rpc::Rpc(std::filesystem::path cookie_file, std::uint16_t port, QObject* parent)
    : QObject(parent),
      worker_(new QObject),
      // As long as bound-cli's: restorewallet rescans the whole chain before it answers.
      client_(std::make_unique<RpcClient>(std::move(cookie_file), port, 900'000)) {
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.start();
}

Rpc::~Rpc() {
    thread_.quit();
    thread_.wait();
}

void Rpc::call(const std::string& method, json params, QObject* context, Done done, Failed failed) {
    QPointer<QObject> ctx(context);
    QMetaObject::invokeMethod(
        worker_,
        [this, method, params = std::move(params), ctx, done = std::move(done), failed = std::move(failed)] {
            json result;
            std::optional<RpcFailure> failure;
            try {
                result = client_->call(method, params);
            } catch (const RpcError& e) {
                failure = RpcFailure{e.code, QString::fromStdString(e.what()), false};
            } catch (const std::exception& e) {
                // Nothing listening, no cookie file yet, or a cookie it refuses: no node to talk to.
                failure = RpcFailure{0, QString::fromStdString(e.what()), true};
            }
            QMetaObject::invokeMethod(
                this,
                [ctx, done, failed, result = std::move(result), failure = std::move(failure)] {
                    if (!ctx) return;
                    if (failure) {
                        if (failed) failed(*failure);
                    } else if (done) {
                        done(result);
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

}  // namespace bound::gui
