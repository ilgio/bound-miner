#include "core/address.h"
#include "core/storage.h"
#include "explorer/explorer.h"
#include "index/chainindex.h"
#include "net/addrman.h"
#include "net/banman.h"
#include "net/peerman.h"
#include "rpc/methods.h"
#include "rpc/server.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

using namespace bound;

namespace {

HttpRequest get(std::string target) {
    HttpRequest request;
    request.method = "GET";
    request.target = std::move(target);
    return request;
}

std::string header_of(const HttpResponse& response, std::string_view name) {
    for (const auto& [n, value] : response.headers) {
        if (n == name) return value;
    }
    return {};
}

bool shows(const HttpResponse& response, const std::string& text) { return response.body.find(text) != std::string::npos; }

const std::string kTxid(64, 'a');
const auto kClock = [] { return kTestNow; };

}  // namespace

TEST(ExplorerText, EscapesHtml) {
    EXPECT_EQ(html_escape("<script>alert('x')</script> & \"q\""), "&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt; &amp; &quot;q&quot;");
    EXPECT_EQ(html_escape("plain text 123"), "plain text 123");
}

TEST(ExplorerText, DecodesUrls) {
    EXPECT_EQ(url_decode("a%20b+c%2F"), "a b c/");
    EXPECT_EQ(url_decode("%2f"), "/");
    EXPECT_EQ(url_decode("%zz"), std::nullopt);
    EXPECT_EQ(url_decode("%2"), std::nullopt);
    EXPECT_EQ(url_decode("%"), std::nullopt);
    const std::map<std::string, std::string> q = parse_query("q=abc%20def&page=3&&bad=%zz&=x&flag&page=4");
    ASSERT_EQ(q.size(), 3u);
    EXPECT_EQ(q.at("q"), "abc def");
    EXPECT_EQ(q.at("page"), "4");  // the later one wins
    EXPECT_EQ(q.at("flag"), "");
}

// A transaction's memo is whatever its sender typed, and a broken node could say anything: all
// of it must reach the page as text, never as markup.
TEST(Explorer, NeverTrustsWhatTheNodeSays) {
    const std::string evil = "<script>alert(1)</script>";
    Explorer explorer(
        [&](const std::string& method, const json&) -> json {
            EXPECT_EQ(method, "getrawtransaction");
            return {{"txid", kTxid},
                    {"confirmations", 0},
                    {"time", kTestNow},
                    {"size", 200},
                    {"fee", evil},
                    {"memo", evil},
                    {"memo_text", evil},
                    {"coinbase", false},
                    {"hex", evil},
                    {"vin", json::array({{{"txid", evil}, {"vout", 0}, {"prevout", {{"address", evil}, {"amount", evil}}}}})},
                    {"vout", json::array({{{"n", 0}, {"address", evil}, {"amount", evil}, {"spent", false}}})}};
        },
        kClock);
    const HttpResponse r = explorer.handle(get("/tx/" + kTxid));
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body.find("<script"), std::string::npos);
    EXPECT_TRUE(shows(r, "&lt;script&gt;alert(1)&lt;/script&gt;"));
    EXPECT_NE(header_of(r, "Content-Security-Policy").find("default-src 'none'"), std::string::npos);
    EXPECT_EQ(header_of(r, "X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(r.content_type, "text/html; charset=utf-8");
}

TEST(Explorer, OnlyAnswersGet) {
    Explorer explorer([](const std::string&, const json&) -> json { return json::object(); }, kClock);
    HttpRequest post = get("/");
    post.method = "POST";
    const HttpResponse refused = explorer.handle(post);
    EXPECT_EQ(refused.status, 405);
    EXPECT_EQ(header_of(refused, "Allow"), "GET, HEAD");
    HttpRequest head = get("/robots.txt");
    head.method = "HEAD";
    EXPECT_EQ(explorer.handle(head).status, 200);
    const HttpResponse robots = explorer.handle(get("/robots.txt"));
    EXPECT_EQ(robots.status, 200);
    EXPECT_EQ(robots.content_type, "text/plain; charset=utf-8");
}

TEST(Explorer, BadUrlsNeverReachTheNode) {
    int calls = 0;
    Explorer explorer(
        [&](const std::string&, const json&) -> json {
            ++calls;
            return nullptr;
        },
        kClock);
    for (const std::string& target :
         {std::string("/nothing"), std::string("/block/xyz"), std::string("/block/1234567890"), std::string("/tx/123"),
          "/tx/" + std::string(64, 'g'), std::string("/address/short"), "/address/" + std::string(40, '<'),
          "/block/" + std::string(65, 'a'), std::string(400, '/'), std::string("no-slash"), std::string("/tx/")}) {
        EXPECT_EQ(explorer.handle(get(target)).status, 404) << target;
    }
    EXPECT_EQ(calls, 0);
}

TEST(Explorer, SaysSoWhenTheNodeIsDownOrRefuses) {
    Explorer down([](const std::string&, const json&) -> json { throw std::runtime_error("connect failed: a detail visitors needn't see"); }, kClock);
    const HttpResponse unreachable = down.handle(get("/"));
    EXPECT_EQ(unreachable.status, 503);
    EXPECT_FALSE(shows(unreachable, "a detail visitors"));

    Explorer missing([](const std::string&, const json&) -> json { throw RpcError(rpc_error::kInvalidAddressOrKey, "no such transaction"); }, kClock);
    EXPECT_EQ(missing.handle(get("/tx/" + kTxid)).status, 404);

    Explorer refusing([](const std::string&, const json&) -> json { throw RpcError(rpc_error::kMisc, "<b>broken</b>"); }, kClock);
    const HttpResponse refused = refusing.handle(get("/mempool"));
    EXPECT_EQ(refused.status, 502);
    EXPECT_FALSE(shows(refused, "<b>broken"));
}

// Milestone 14: /health, for uptime monitors.
TEST(Explorer, HealthSaysWhatsWrong) {
    std::int64_t tip_time = kTestNow - 600;
    std::int64_t peers = 3;
    bool node_up = true;
    ExplorerOptions options;
    options.stale_after = 3600;
    options.cache_seconds = 0;
    Explorer explorer(
        [&](const std::string& method, const json&) -> json {
            if (!node_up) throw std::runtime_error("connection refused");
            if (method == "getblockchaininfo") return {{"blocks", 120}, {"time", tip_time}};
            if (method == "getnetworkinfo") return {{"connections", peers}};
            ADD_FAILURE() << "unexpected call " << method;
            return {};
        },
        kClock, options);
    const auto health = [&] {
        const HttpResponse r = explorer.handle(get("/health"));
        EXPECT_EQ(r.content_type, "application/json");
        EXPECT_EQ(header_of(r, "Cache-Control"), "no-store");
        return std::pair(r.status, json::parse(r.body));
    };

    auto [status, report] = health();
    EXPECT_EQ(status, 200);
    EXPECT_EQ(report["status"], "ok");
    EXPECT_EQ(report["network"], "testnet");
    EXPECT_EQ(report["height"], 120);
    EXPECT_EQ(report["tip_age"], 600);
    EXPECT_EQ(report["peers"], 3);
    EXPECT_FALSE(report.contains("problem"));

    tip_time = kTestNow - 7200;
    std::tie(status, report) = health();
    EXPECT_EQ(status, 503);
    EXPECT_EQ(report["status"], "unhealthy");
    EXPECT_EQ(report["problem"], "no block for 120 minutes");

    peers = 0;
    std::tie(status, report) = health();
    EXPECT_EQ(status, 503);
    EXPECT_EQ(report["problem"], "the node has no peers");

    node_up = false;
    std::tie(status, report) = health();
    EXPECT_EQ(status, 503);
    EXPECT_EQ(report["problem"], "the node isn't answering");
    EXPECT_FALSE(report.contains("height"));

    // Twelve blocks' time, never under an hour.
    EXPECT_EQ(default_stale_after(60), 3600);
    EXPECT_EQ(default_stale_after(600), 7200);
}

TEST(Explorer, BuildsEachPageOncePerCachePeriod) {
    int calls = 0;
    std::int64_t now = kTestNow;
    Explorer explorer(
        [&](const std::string&, const json&) -> json {
            ++calls;
            return json::object();
        },
        [&] { return now; });
    explorer.handle(get("/mempool"));
    const int once = calls;
    ASSERT_GT(once, 0);
    explorer.handle(get("/mempool"));
    explorer.handle(get("/mempool?x=1"));  // parameters that mean nothing don't make it another page
    explorer.handle(get("/mempool?x=2&y=3"));
    EXPECT_EQ(calls, once);
    now += 2;
    explorer.handle(get("/mempool"));
    EXPECT_EQ(calls, 2 * once);
}

namespace {

// The explorer over a real node (without sockets): a wallet that has mined 101 blocks, and the index.
class ExplorerNodeTest : public ::testing::Test {
protected:
    ExplorerNodeTest() {
        node.wallet_options.kdf = kTestKdf;
        register_node_methods(table, node);
        rpc("createwallet", {"secret"});
        miner = rpc("getnewaddress").get<std::string>();
        rpc("generatetoaddress", {101, miner});
        index = std::make_unique<ChainIndex>(index_db, chain);
        node.index = index.get();
        rpc("walletpassphrase", {"secret", 600});
    }

    json rpc(const std::string& method, json args = json::array()) {
        const json reply = table.call(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", args}});
        if (reply.contains("error")) throw RpcError(reply["error"]["code"].get<int>(), reply["error"]["message"].get<std::string>());
        return reply["result"];
    }
    HttpResponse page(const std::string& target) { return explorer.handle(get(target)); }

    const ConsensusParams params = easy_params();
    Chainstate chain{params};
    Mempool mempool{chain};
    AddrMan addrman{1};
    BanMan banman;
    PeerManager peers{chain, mempool, addrman, banman, {}, 1};
    MemoryStore wallet_db;
    MemoryStore index_db;
    NodeContext node{.chain = chain, .mempool = mempool, .peers = peers, .clock = [] { return kTestNow; }, .wallet_db = &wallet_db};
    RpcTable table;
    std::unique_ptr<ChainIndex> index;
    std::string miner;
    std::int64_t now = kTestNow;
    Explorer explorer{[this](const std::string& method, const json& args) { return rpc(method, args); }, [this] { return now; }};
};

}  // namespace

TEST_F(ExplorerNodeTest, TheFrontPageShowsTheChain) {
    const HttpResponse r = page("/");
    ASSERT_EQ(r.status, 200);
    const std::string tip = rpc("getbestblockhash").get<std::string>();
    EXPECT_TRUE(shows(r, "/block/" + tip));
    EXPECT_TRUE(shows(r, "<b>101</b>"));
    EXPECT_TRUE(shows(r, "505.00000000 BOUND"));
    // The 15 latest blocks: #87 is on it, #86 isn't.
    EXPECT_TRUE(shows(r, rpc("getblockhash", {87}).get<std::string>()));
    EXPECT_FALSE(shows(r, rpc("getblockhash", {86}).get<std::string>()));
}

TEST_F(ExplorerNodeTest, APaymentFromTheMempoolToABlock) {
    const std::string bob = encode_address(test_key("bob").public_key(), params.address_version);
    const std::string txid = rpc("sendtoaddress", {bob, "1.25", 2, "thanks <3"}).get<std::string>();  // 200 bytes at 2 per byte

    EXPECT_TRUE(shows(page("/mempool"), "/tx/" + txid));
    HttpResponse r = page("/tx/" + txid);
    ASSERT_EQ(r.status, 200);
    EXPECT_TRUE(shows(r, "Waiting in the"));
    EXPECT_TRUE(shows(r, "/address/" + bob));
    EXPECT_TRUE(shows(r, "/address/" + miner));  // where its input came from
    EXPECT_TRUE(shows(r, "1.25000000 BOUND"));
    EXPECT_TRUE(shows(r, "0.00000400 BOUND"));
    EXPECT_TRUE(shows(r, "thanks &lt;3"));
    r = page("/search?q=" + txid);
    EXPECT_EQ(r.status, 302);
    EXPECT_EQ(header_of(r, "Location"), "/tx/" + txid);

    const std::string spent = rpc("getrawtransaction", {txid, 2})["vin"][0]["txid"].get<std::string>();
    rpc("generatetoaddress", {1, miner});
    now += 10;  // past the cache
    r = page("/tx/" + txid);
    EXPECT_TRUE(shows(r, "In block"));
    EXPECT_TRUE(shows(r, "1 confirmation,"));
    EXPECT_TRUE(shows(r, "Unspent"));

    r = page("/block/102");
    ASSERT_EQ(r.status, 200);
    EXPECT_TRUE(shows(r, "Block 102"));
    EXPECT_TRUE(shows(r, "/tx/" + txid));
    EXPECT_TRUE(shows(r, "0.00000400 BOUND"));
    const std::string tip = rpc("getbestblockhash").get<std::string>();
    EXPECT_EQ(page("/block/" + tip).body, r.body);  // by hash or by height: the same page

    r = page("/address/" + bob);
    EXPECT_TRUE(shows(r, "1.25000000 BOUND"));
    EXPECT_TRUE(shows(r, "/tx/" + txid));
    EXPECT_TRUE(shows(r, "Page 1 of 1"));

    r = page("/tx/" + spent);  // the coinbase it spent: found through the index, and marked spent
    EXPECT_TRUE(shows(r, "Coinbase"));
    EXPECT_TRUE(shows(r, "Spent by"));
    EXPECT_TRUE(shows(r, "/tx/" + txid));

    EXPECT_EQ(header_of(page("/search?q=102"), "Location"), "/block/102");
    EXPECT_EQ(header_of(page("/search?q=" + tip), "Location"), "/block/" + tip);
    EXPECT_EQ(header_of(page("/search?q=+" + bob + "%20"), "Location"), "/address/" + bob);
    EXPECT_EQ(page("/search?q=nonsense").status, 404);
    EXPECT_EQ(page("/search?q=" + std::string(64, 'e')).status, 404);
    EXPECT_EQ(page("/search").status, 200);
    EXPECT_EQ(page("/block/999999").status, 404);
    EXPECT_EQ(page("/tx/" + std::string(64, 'e')).status, 404);
    EXPECT_EQ(page("/address/" + std::string(51, '9')).status, 404);  // the right shape, not a real address
}

TEST_F(ExplorerNodeTest, AnAddresssHistoryIsPaged) {
    // The miner's address has 101 coinbases: 25 a page.
    HttpResponse r = page("/address/" + miner);
    ASSERT_EQ(r.status, 200);
    EXPECT_TRUE(shows(r, "Page 1 of 5"));
    EXPECT_TRUE(shows(r, "505.00000000 BOUND"));
    EXPECT_TRUE(shows(r, "?page=2"));
    EXPECT_FALSE(shows(r, "?page=0"));
    EXPECT_TRUE(shows(r, "/block/101\""));
    EXPECT_TRUE(shows(r, "/block/77\""));
    EXPECT_FALSE(shows(r, "/block/76\""));

    r = page("/address/" + miner + "?page=5");
    EXPECT_TRUE(shows(r, "Page 5 of 5"));
    EXPECT_TRUE(shows(r, "/block/1\""));
    EXPECT_FALSE(shows(r, "/block/2\""));
    EXPECT_FALSE(shows(r, "?page=6"));
    EXPECT_TRUE(shows(page("/address/" + miner + "?page=99"), "Page 5 of 5"));  // past the end: the last page
    EXPECT_TRUE(shows(page("/address/" + miner + "?page=junk"), "Page 1 of 5"));
}

TEST_F(ExplorerNodeTest, WithoutAnIndexItSaysWhatsMissing) {
    node.index = nullptr;
    const HttpResponse r = page("/address/" + miner);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(shows(r, "--index"));
    EXPECT_FALSE(shows(r, "Balance"));
}
