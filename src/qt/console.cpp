#include "qt/format.h"
#include "qt/nodehost.h"
#include "qt/pages.h"
#include "qt/style.h"

#include "rpc/client.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

namespace bound::gui {

namespace {

// Methods whose parameters are secrets: echoed without them, and kept out of the history.
bool secret(const std::string& method) {
    return method == "walletpassphrase" || method == "walletpassphrasechange" || method == "createwallet" ||
           method == "restorewallet";
}

QPlainTextEdit* text_area(QWidget* parent) {
    auto* area = new QPlainTextEdit(parent);
    area->setReadOnly(true);
    area->setFont(mono_font());
    area->setFrameShape(QFrame::NoFrame);
    return area;
}

}  // namespace

ConsolePage::ConsolePage(Session& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(28, 24, 28, 24);
    page->setSpacing(14);
    page->addWidget(page_title(QStringLiteral("Console"), this));
    tabs_ = new QTabWidget(this);

    auto* console = new QWidget(tabs_);
    auto* c = new QVBoxLayout(console);
    c->setContentsMargins(12, 12, 12, 12);
    c->addWidget(warning_banner(QStringLiteral("Commands go straight to your node, like bound-cli: try <b>help</b>. "
                                               "Never type one someone else gives you. <b>dumpseed</b> shows the key "
                                               "to all your coins, and scammers ask for exactly that."),
                                console));
    output_ = text_area(console);
    c->addWidget(output_, 1);
    input_ = new QLineEdit(console);
    input_->setFont(mono_font());
    input_->setPlaceholderText(QStringLiteral("getblockchaininfo"));
    input_->installEventFilter(this);
    c->addWidget(input_);
    tabs_->addTab(console, QStringLiteral("Console"));

    log_ = text_area(tabs_);
    log_->setMaximumBlockCount(5000);
    tabs_->addTab(log_, QStringLiteral("Node log"));
    page->addWidget(tabs_, 1);

    connect(input_, &QLineEdit::returnPressed, this, [this] {
        const QString line = input_->text().trimmed();
        input_->clear();
        if (!line.isEmpty()) run(line);
    });
    print(QStringLiteral("<span style='color:%1'>BOUND node console. <b>help</b> lists the commands, <b>help "
                         "&lt;command&gt;</b> explains one, <b>clear</b> empties this window.</span>")
              .arg(colors().muted.name()));
    if (!session_.host().embedded()) {
        append_log(QStringLiteral("This app is using a node started elsewhere (boundd): its log is that node's output."));
    }
}

void ConsolePage::append_log(const QString& line) {
    QScrollBar* bar = log_->verticalScrollBar();
    const bool at_end = bar->value() == bar->maximum();
    log_->appendPlainText(line);
    if (at_end) bar->setValue(bar->maximum());
}

bool ConsolePage::eventFilter(QObject* watched, QEvent* event) {
    if (watched == input_ && event->type() == QEvent::KeyPress && !history_.isEmpty()) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            history_pos_ = std::clamp(history_pos_ + (key == Qt::Key_Up ? -1 : 1), 0, static_cast<int>(history_.size()));
            input_->setText(history_pos_ < history_.size() ? history_[history_pos_] : QString());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ConsolePage::print(const QString& html) {
    output_->appendHtml(html);
    output_->verticalScrollBar()->setValue(output_->verticalScrollBar()->maximum());
}

void ConsolePage::run(const QString& line) {
    if (line == QLatin1String("clear")) {
        output_->clear();
        return;
    }
    std::vector<std::string> words;
    try {
        words = split_words(line.toStdString());
    } catch (const std::exception& e) {
        print(QStringLiteral("<span style='color:%1'>%2</span>").arg(colors().negative.name(), QString::fromStdString(e.what()).toHtmlEscaped()));
        return;
    }
    if (words.empty()) return;
    const std::string method = words.front();
    const bool hide = secret(method);
    const QString shown = hide ? QString::fromStdString(method) + QStringLiteral(" (parameters hidden)") : line;
    if (!hide) history_.push_back(line);
    history_pos_ = static_cast<int>(history_.size());
    print(QStringLiteral("<b>&gt; %1</b>").arg(shown.toHtmlEscaped()));

    json params = json::array();
    try {
        for (std::size_t i = 1; i < words.size(); ++i) params.push_back(cli_param(method, i - 1, words[i]));
    } catch (const std::exception& e) {
        print(QStringLiteral("<span style='color:%1'>%2</span>").arg(colors().negative.name(), QString::fromStdString(e.what()).toHtmlEscaped()));
        return;
    }
    session_.rpc().call(
        method, params, this,
        [this](const json& result) {
            QString text;
            if (result.is_string()) {
                text = QString::fromStdString(result.get<std::string>());
            } else if (!result.is_null()) {
                text = QString::fromStdString(dump_json(result, 2));
            }
            if (!text.isEmpty()) print(QStringLiteral("<pre>%1</pre>").arg(text.toHtmlEscaped()));
            session_.refresh();
        },
        [this](const RpcFailure& f) {
            print(QStringLiteral("<span style='color:%1'>error%2: %3</span>")
                      .arg(colors().negative.name(), f.code != 0 ? QStringLiteral(" %1").arg(f.code) : QString(), f.message.toHtmlEscaped()));
        });
}

}  // namespace bound::gui
