// Milestone 9: the wallet app (bound-qt) against a real node running inside it, drawn off screen.
// With BOUND_SCREENSHOTS=<dir> it also saves a picture of each page there.
#include "app/datadir.h"
#include "core/address.h"
#include "core/amount.h"
#include "core/params.h"
#include "net/socket.h"
#include "node/node.h"
#include "qt/contacts.h"
#include "qt/format.h"
#include "qt/icons.h"
#include "qt/mainwindow.h"
#include "qt/nodehost.h"
#include "qt/qr.h"
#include "qt/style.h"
#include "rpc/client.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QTableWidget>
#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace bound;
using namespace bound::gui;

namespace {

std::uint16_t free_port() { return Socket::listen(0, true).local_port(); }

// Runs the event loop until `done` holds, or `ms` pass.
bool wait_until(const std::function<bool()>& done, int ms = 30'000) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return true;
}

bool shows_text(const QWidget* root, const QString& text) {
    for (const QLabel* label : root->findChildren<QLabel*>()) {
        if (label->text() == text) return true;
    }
    return false;
}

void screenshot(MainWindow& window, MainWindow::Page page, const char* name) {
    const QString dir = qEnvironmentVariable("BOUND_SCREENSHOTS");
    if (dir.isEmpty()) return;
    window.show_page(page);
    wait_until([] { return false; }, 2500);  // a refresh for this page's own data
    QDir().mkpath(dir);
    window.grab().save(dir + QStringLiteral("/") + QString::fromLatin1(name) + QStringLiteral(".png"));
}

}  // namespace

TEST(WalletApp, FormatsWhatTheNodeSays) {
    // Milestone 18: two decimals at least, and no zeros after them that say nothing.
    EXPECT_EQ(coins_text(150'000'000), QStringLiteral("1.50 BOUND"));
    EXPECT_EQ(coins_text(5 * kCoin, true), QStringLiteral("+5.00 BOUND"));
    EXPECT_EQ(coins_text(-191, true), QStringLiteral("-0.00000191 BOUND"));
    EXPECT_EQ(coins_text(0), QStringLiteral("0.00 BOUND"));
    EXPECT_EQ(amount_text(2'774'999'709), QStringLiteral("27.74999709"));
    EXPECT_EQ(amount_text(123'450'000), QStringLiteral("1.2345"));
    EXPECT_EQ(short_hash(QStringLiteral("0000a7ba8cc2ed04fa56bb3eaf23d5879874e7ffd060d81a2aca994f79b79293")),
              QStringLiteral("0000a7ba") + QChar(0x2026) + QStringLiteral("79293"));
    EXPECT_EQ(hashrate_text(1'234'567), QStringLiteral("1.23 MH/s"));
    EXPECT_EQ(hashrate_text(950), QStringLiteral("950 H/s"));
    EXPECT_EQ(age_text(30), QStringLiteral("30 s"));
    EXPECT_EQ(age_text(7200), QStringLiteral("2 h"));
    EXPECT_EQ(category_text(QStringLiteral("generate")), QStringLiteral("Mined"));
    EXPECT_EQ(amount_of(json{{"a", "0.00000420"}}, "a"), std::optional<Amount>(420));
    EXPECT_EQ(amount_of(json{{"a", 1}}, "a"), std::nullopt);
    EXPECT_EQ(int_of(json{{"n", 7}}, "n"), 7);
    EXPECT_EQ(text_of(json{{"s", "x"}, {"n", 3}}, "n"), QStringLiteral("3"));
}

// Milestone 18: bound: links, as the Receive page makes them and Send reads them.
TEST(WalletApp, PaymentLinks) {
    const QString to = QStringLiteral("BW1example");
    EXPECT_EQ(payment_link(to), QStringLiteral("bound:BW1example"));
    EXPECT_EQ(payment_link(to, 150'000'000), QStringLiteral("bound:BW1example?amount=1.5"));
    EXPECT_EQ(payment_link(to, 5 * kCoin), QStringLiteral("bound:BW1example?amount=5"));
    EXPECT_EQ(payment_link(to, 191), QStringLiteral("bound:BW1example?amount=0.00000191"));

    const std::optional<PaymentRequest> read = parse_payment_link(QStringLiteral("  BOUND:BW1example?label=Alice&amount=1.5 "));
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->address, to);
    EXPECT_EQ(read->amount, std::optional<Amount>(150'000'000));
    EXPECT_EQ(parse_payment_link(QStringLiteral("bound:BW1example"))->amount, std::nullopt);
    EXPECT_FALSE(parse_payment_link(to).has_value());  // a bare address isn't a link
    EXPECT_FALSE(parse_payment_link(QStringLiteral("bound:")).has_value());
    EXPECT_FALSE(parse_payment_link(QStringLiteral("bound:BW1example?amount=lots")).has_value());
    EXPECT_FALSE(parse_payment_link(QStringLiteral("bound:BW1example?amount=-1")).has_value());
    EXPECT_FALSE(parse_payment_link(QStringLiteral("bound:BW1example?req-signature=x")).has_value());  // BIP21's rule

    EXPECT_EQ(explorer_tx_url("mainnet", QStringLiteral("ab")), std::optional<QString>(QStringLiteral("https://explorer.bound.sarl/tx/ab")));
    EXPECT_EQ(explorer_tx_url("testnet", QStringLiteral("ab")),
              std::optional<QString>(QStringLiteral("https://testnet-explorer.bound.sarl/tx/ab")));
    EXPECT_EQ(explorer_tx_url("regtest", QStringLiteral("ab")), std::nullopt);
}

// Milestone 18: contacts live in the app's settings file, sorted by name.
TEST(WalletApp, ContactsAreKeptInTheSettingsFile) {
    TempDir dir;
    const QString file = QString::fromStdU16String((dir.path() / "bound-qt.ini").u16string());
    const auto contact = [](const char* name, const char* address) { return Contact{QString::fromLatin1(name), QString::fromLatin1(address)}; };
    {
        Contacts contacts(file);
        EXPECT_TRUE(contacts.list().empty());
        contacts.put(contact("bob", "B2"));
        contacts.put(contact("Alice", "B1"));
        contacts.put(contact("  ", "B3"));  // no name: not saved
        EXPECT_EQ(contacts.list(), (std::vector<Contact>{contact("Alice", "B1"), contact("bob", "B2")}));
        contacts.put(contact("Robert", "B2"));  // the same address: renamed
        EXPECT_EQ(contacts.name_for(QStringLiteral("B2")), std::optional<QString>(QStringLiteral("Robert")));
        EXPECT_EQ(contacts.version(), 3);
    }
    Contacts again(file);
    EXPECT_EQ(again.list(), (std::vector<Contact>{contact("Alice", "B1"), contact("Robert", "B2")}));
    again.remove(QStringLiteral("B1"));
    EXPECT_EQ(again.name_for(QStringLiteral("B1")), std::nullopt);
    EXPECT_EQ(Contacts(file).list(), (std::vector<Contact>{contact("Robert", "B2")}));
}

// Milestone 18: the Receive page's QR code, checked against the parts every QR code has (ISO/IEC
// 18004): its size, the three finder squares, the timing lines, and the format bits, which must
// say error correction level M under a valid BCH code.
TEST(WalletApp, QrCodesFollowTheStandard) {
    const QString text = payment_link(QString::fromStdString(encode_address(test_key("alice").public_key(), mainnet_params().address_version)),
                                      150'000'000);
    const std::vector<std::vector<bool>> m = qr_modules(text);
    const int n = static_cast<int>(m.size());
    ASSERT_GE(n, 21);
    ASSERT_EQ((n - 17) % 4, 0);  // 17 + 4 x version
    const auto dark = [&](int x, int y) { return static_cast<bool>(m[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)]); };
    for (const auto [cx, cy] : {std::pair{0, 0}, std::pair{n - 7, 0}, std::pair{0, n - 7}}) {
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 7; ++x) {
                const int ring = std::max(std::abs(x - 3), std::abs(y - 3));
                EXPECT_EQ(dark(cx + x, cy + y), ring != 2) << "finder at " << cx << "," << cy;
            }
        }
    }
    for (int i = 8; i < n - 8; ++i) {
        EXPECT_EQ(dark(i, 6), i % 2 == 0);
        EXPECT_EQ(dark(6, i), i % 2 == 0);
    }
    EXPECT_TRUE(dark(8, n - 8));  // the module that's always dark
    // The 15 format bits beside the top-left finder: (level << 3 | mask), its BCH(15,5) remainder,
    // XORed with 0x5412. Level M is 00.
    int bits = 0;
    for (int i = 0; i <= 5; ++i) bits |= (dark(8, i) ? 1 : 0) << i;
    bits |= (dark(8, 7) ? 1 : 0) << 6;
    bits |= (dark(8, 8) ? 1 : 0) << 7;
    bits |= (dark(7, 8) ? 1 : 0) << 8;
    for (int i = 9; i < 15; ++i) bits |= (dark(14 - i, 8) ? 1 : 0) << i;
    bool matched = false;
    for (int mask = 0; mask < 8; ++mask) {
        const int data = (0 << 3) | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        if (((data << 10 | rem) ^ 0x5412) == bits) matched = true;
    }
    EXPECT_TRUE(matched) << "format bits " << bits;

    // The picture: 4 white modules around it, then the finder's black corner.
    const QImage image = qr_image(text, 5);
    ASSERT_EQ(image.width(), (n + 8) * 5);
    EXPECT_EQ(image.pixelColor(0, 0), QColor(Qt::white));
    EXPECT_EQ(image.pixelColor(4 * 5 - 1, 4 * 5 - 1), QColor(Qt::white));
    EXPECT_EQ(image.pixelColor(4 * 5, 4 * 5), QColor(Qt::black));
    EXPECT_THROW(qr_modules(QString(3000, QLatin1Char('x'))), std::length_error);
}

TEST(WalletApp, IconsAreDrawn) {
    for (const Glyph g : {Glyph::Overview, Glyph::Send, Glyph::Receive, Glyph::Contacts, Glyph::Transactions, Glyph::Mining, Glyph::Network,
                          Glyph::Console, Glyph::Received, Glyph::Sent, Glyph::Mined}) {
        const QImage image = glyph_pixmap(g, Qt::black, 24).toImage();
        int inked = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) inked += image.pixelColor(x, y).alpha() > 0 ? 1 : 0;
        }
        EXPECT_GT(inked, 20) << static_cast<int>(g);
        EXPECT_LT(inked, image.width() * image.height() / 2) << static_cast<int>(g);  // lines, not a blot
    }
}

TEST(WalletApp, ConsoleWordsAreSplitLikeAShell) {
    EXPECT_EQ(split_words("help"), (std::vector<std::string>{"help"}));
    EXPECT_EQ(split_words("  sendtoaddress  9abc 1.5 "), (std::vector<std::string>{"sendtoaddress", "9abc", "1.5"}));
    EXPECT_EQ(split_words(R"(walletpassphrase "two words" 60)"), (std::vector<std::string>{"walletpassphrase", "two words", "60"}));
    EXPECT_EQ(split_words(R"(a "" b)"), (std::vector<std::string>{"a", "", "b"}));
    EXPECT_EQ(split_words(R"(say \"hi\" \\)"), (std::vector<std::string>{"say", "\"hi\"", "\\"}));
    EXPECT_THROW(split_words(R"(a "open)"), std::invalid_argument);
    EXPECT_EQ(cli_param("setmining", 0, "true"), json(true));
    EXPECT_EQ(cli_param("sendtoaddress", 2, "2.5"), json(2.5));
    EXPECT_EQ(cli_param("sendtoaddress", 1, "2.5"), json("2.5"));
    EXPECT_THROW(cli_param("setmining", 0, "yes"), std::invalid_argument);
}

// The whole app: a node inside it, a wallet, mining switched on from the app, and the pages
// showing what happened. Then closing the window stops the node.
TEST(WalletApp, RunsANodeInsideAndShowsItsWallet) {
    TempDir dir;
    NodeOptions options;
    // Milestone 16: regtest, whose small BoundHash mines a block in a moment even in Debug; a
    // testnet block takes a Debug build minutes now.
    options.network = Network::Regtest;
    options.listen = false;
    options.local = true;
    options.use_seeds = false;  // never the real network
    options.rpc_port = free_port();
    NodeHost host(dir.path(), options);
    host.start();
    ASSERT_TRUE(host.embedded());

    RpcClient rpc(cookie_path(dir.path()), options.rpc_port, 60'000);
    const auto answers = [&rpc] {
        try {
            rpc.call("getblockcount");
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    ASSERT_TRUE(wait_until(answers));
    rpc.call("createwallet", json::array({"correct horse battery"}));

    {
        MainWindow window(host);
        window.show();
        ASSERT_TRUE(wait_until([&] { return window.connected(); }));
        screenshot(window, MainWindow::Overview, "1-overview-empty");

        // Mining is switched on and off through the node, as the Mining page does.
        const json on = rpc.call("setmining", json::array({true}));
        EXPECT_EQ(on["mining"], true);
        EXPECT_TRUE(on["address"].is_string());
        ASSERT_TRUE(wait_until([&] { return rpc.call("getblockcount").get<int>() >= 3; }, 120'000));
        const json off = rpc.call("setmining", json::array({false}));
        EXPECT_EQ(off["mining"], false);
        EXPECT_GE(off["blocksfound"].get<int>(), 3);
        const int height = rpc.call("getblockcount").get<int>();

        // The overview shows the mined coins as maturing, and the list has every block's coinbase.
        const json wallet = rpc.call("getwalletinfo");
        const QString maturing = coins_text(parse_coins(wallet["immature_balance"].get<std::string>()).value());
        EXPECT_EQ(maturing, coins_text(Amount{height} * block_subsidy(1, regtest_params())));
        window.refresh();
        EXPECT_TRUE(wait_until([&] { return shows_text(window.page(MainWindow::Overview), maturing); }));
        auto* table = window.page(MainWindow::Transactions)->findChild<QTableWidget*>();
        ASSERT_NE(table, nullptr);
        EXPECT_TRUE(wait_until([&] { return table->rowCount() == height; }));
        EXPECT_EQ(table->item(0, 1)->text(), QStringLiteral("Mined (maturing)"));
        EXPECT_TRUE(table->item(0, 2)->text().startsWith(QLatin1Char('9')));
        // The height comes from its own call in the refresh, which can be a refresh behind the list.
        EXPECT_TRUE(wait_until([&] { return shows_text(window.page(MainWindow::Overview), QStringLiteral("#%1").arg(height)); }));

        if (!qEnvironmentVariable("BOUND_SCREENSHOTS").isEmpty()) {
            // For the pictures, a wallet with some history: coins old enough to spend, and a payment.
            rpc.call("setmining", json::array({true}));
            ASSERT_TRUE(wait_until([&] { return rpc.call("getblockcount").get<int>() >= 106; }, 600'000));
            rpc.call("setmining", json::array({false}));
            rpc.call("walletpassphrase", json::array({"correct horse battery", 60}));
            rpc.call("sendtoaddress", json::array({encode_address(test_key("bob").public_key(), regtest_params().address_version), "7.25"}));
            rpc.call("walletlock");
            window.refresh();
        }
        screenshot(window, MainWindow::Overview, "2-overview");
        screenshot(window, MainWindow::Send, "3-send");
        screenshot(window, MainWindow::Receive, "4-receive");
        screenshot(window, MainWindow::Contacts, "4b-contacts");
        screenshot(window, MainWindow::Transactions, "5-transactions");
        screenshot(window, MainWindow::Mining, "6-mining");
        screenshot(window, MainWindow::Network, "7-network");
        screenshot(window, MainWindow::Console, "8-console");

        // Closing the window stops the node inside, and waits for it.
        window.close();
        EXPECT_TRUE(wait_until([&] { return !host.running(); }));
        EXPECT_TRUE(wait_until([&] { return !window.isVisible(); }));
    }
    EXPECT_FALSE(answers());
}

// A second app on the same data directory uses the node that's already running.
TEST(WalletApp, UsesANodeAlreadyRunning) {
    TempDir dir;
    NodeOptions options;
    options.listen = false;
    options.local = true;
    options.use_seeds = false;
    options.rpc_port = free_port();
    NodeHost first(dir.path(), options);
    first.start();
    ASSERT_TRUE(first.embedded());
    RpcClient rpc(cookie_path(dir.path()), options.rpc_port);
    ASSERT_TRUE(wait_until([&rpc] {
        try {
            rpc.call("getblockcount");
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }));
    NodeHost second(dir.path(), options);
    second.start();
    EXPECT_FALSE(second.embedded());
    EXPECT_FALSE(second.running());
}

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", BOUND_TEST_PLATFORM);
    QApplication app(argc, argv);
    apply_style(app);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
