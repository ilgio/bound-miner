#include "core/address.h"
#include "core/amount.h"
#include "net/addrman.h"
#include "net/peerman.h"
#include "rpc/http.h"
#include "rpc/methods.h"
#include "rpc/server.h"
#include "test_util.h"
#include "util/base64.h"
#include "wallet/mnemonic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace bound;

TEST(Amounts, FormatAndParseExactly) {
    EXPECT_EQ(format_coins(150'000'000), "1.50000000");
    EXPECT_EQ(format_coins(-191), "-0.00000191");
    EXPECT_EQ(format_coins(0), "0.00000000");
    EXPECT_EQ(format_coins(kMaxMoney), "21000000.00000000");
    EXPECT_EQ(parse_coins("1.5"), 150'000'000);
    EXPECT_EQ(parse_coins("0.00000191"), 191);
    EXPECT_EQ(parse_coins("7"), 7 * kCoin);
    EXPECT_EQ(parse_coins("-2.25"), -225'000'000);
    EXPECT_EQ(parse_coins("21000000"), kMaxMoney);
    for (const std::string_view bad : {"", "1.", ".5", "1.123456789", "1e5", " 1", "+1", "21000000.00000001", "abc", "1.2.3", "-",
                                       "123456789", "0x10"}) {
        EXPECT_EQ(parse_coins(bad), std::nullopt) << bad;
    }
}

TEST(Base64, Rfc4648Vectors) {
    const std::pair<std::string_view, std::string_view> vectors[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
    };
    for (const auto& [plain, encoded] : vectors) {
        EXPECT_EQ(base64_encode(bytes_of(plain)), encoded);
        const std::optional<Bytes> decoded = base64_decode(encoded);
        ASSERT_TRUE(decoded.has_value()) << encoded;
        EXPECT_EQ(std::string(decoded->begin(), decoded->end()), plain);
    }
    const Bytes all = [] {
        Bytes b;
        for (int i = 0; i < 256; ++i) b.push_back(static_cast<std::uint8_t>(i));
        return b;
    }();
    EXPECT_EQ(base64_decode(base64_encode(all)), all);
    for (const std::string_view bad : {"Zg", "Zg=", "Z===", "Zh==", "Zm9=", "Zm=v", "Zm9v!A==", "====", "Zg==Zg=="}) {
        EXPECT_EQ(base64_decode(bad), std::nullopt) << bad;
    }
}

TEST(Http, ReadsRequestsAsTheyArrive) {
    HttpRequestReader reader;
    reader.feed(bytes_of("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nAuthorization:  Basic abc \r\n\r\nhel"));
    EXPECT_FALSE(reader.next().has_value());
    reader.feed(bytes_of("loGET /x HTTP/1.0\r\n\r\nPOST / HTTP/1.1\r\nConnection: Close\r\n\r\n"));
    const std::optional<HttpRequest> a = reader.next();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->method, "POST");
    EXPECT_EQ(a->target, "/");
    EXPECT_EQ(a->body, "hello");
    EXPECT_EQ(a->header("authorization"), "Basic abc");
    EXPECT_TRUE(a->keep_alive);
    const std::optional<HttpRequest> b = reader.next();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->method, "GET");
    EXPECT_EQ(b->target, "/x");
    EXPECT_FALSE(b->keep_alive);  // HTTP/1.0 without "keep-alive"
    const std::optional<HttpRequest> c = reader.next();
    ASSERT_TRUE(c.has_value());
    EXPECT_FALSE(c->keep_alive);
    EXPECT_FALSE(reader.next().has_value());
}

TEST(Http, RefusesBadRequests) {
    const auto status = [](std::string_view text) {
        HttpRequestReader reader;
        reader.feed(bytes_of(text));
        try {
            reader.next();
            return 0;
        } catch (const HttpError& e) {
            return e.status;
        }
    };
    EXPECT_EQ(status("GARBAGE\r\n\r\n"), 400);
    EXPECT_EQ(status("POST / HTTP/2\r\n\r\n"), 400);
    EXPECT_EQ(status("POST /  HTTP/1.1\r\n\r\n"), 400);
    EXPECT_EQ(status("POST / HTTP/1.1\r\nno colon\r\n\r\n"), 400);
    EXPECT_EQ(status("POST / HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n"), 413);
    EXPECT_EQ(status("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n"), 413);
    EXPECT_EQ(status("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"), 501);
    EXPECT_EQ(status(std::string(20000, 'a')), 431);
    EXPECT_EQ(status("POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nab"), 0);  // waits for the rest
}

TEST(Http, FormatsAndParsesResponses) {
    const std::string text = format_http_response({200, "{}", "application/json", {{"X-A", "b"}}}, false);
    EXPECT_EQ(text, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\nX-A: b\r\nConnection: close\r\n\r\n{}");
    const std::optional<HttpResponse> parsed = parse_http_response(text, false);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->status, 200);
    EXPECT_EQ(parsed->body, "{}");
    EXPECT_FALSE(parse_http_response(std::string_view(text).substr(0, text.size() - 1), false).has_value());
    EXPECT_THROW(parse_http_response(std::string_view(text).substr(0, text.size() - 1), true), std::runtime_error);
    EXPECT_THROW(parse_http_response("SMTP ready\r\n\r\n", false), std::runtime_error);
}

// Milestone 8, for a server strangers reach: a connection that dawdles over its request is dropped.
TEST(Http, SlowRequestsAreDropped) {
    HttpServerOptions options;
    options.request_timeout = 10;
    HttpServer server(options, [](const HttpRequest&) { return HttpResponse{200, "ok", "text/plain", {}}; });
    const auto step = [&](std::int64_t now) {
        std::vector<PollItem> items;
        server.add_poll_items(items);
        poll(items, 20);
        server.handle_poll(items, now);
    };
    const Socket client = Socket::connect(NetAddress::ipv4(127, 0, 0, 1, server.port()));
    for (int i = 0; i < 100 && server.connections() == 0; ++i) step(kTestNow);
    ASSERT_EQ(server.connections(), 1u);

    // HEAD: the same headers as GET, Content-Length included, and no body.
    const Socket head = Socket::connect(NetAddress::ipv4(127, 0, 0, 1, server.port()));
    for (int i = 0; i < 100 && server.connections() < 2; ++i) step(kTestNow);
    const std::string_view ask = "HEAD / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(head.send(bytes_of(ask)).bytes, ask.size());
    std::string reply;
    std::array<std::uint8_t, 512> buffer;
    for (int i = 0; i < 200; ++i) {
        step(kTestNow);
        const IoResult r = head.recv(buffer);
        if (r.closed) break;  // the server hung up, as asked, after the reply
        if (!r.would_block) reply.append(reinterpret_cast<const char*>(buffer.data()), r.bytes);
    }
    EXPECT_TRUE(reply.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(reply.find("Content-Length: 2\r\n"), std::string::npos);
    EXPECT_TRUE(reply.ends_with("\r\n\r\n"));  // and nothing after

    const std::string_view half = "GET / HTTP/1.1\r\nHost: x\r\n";  // no blank line: the request isn't finished
    ASSERT_EQ(client.send(bytes_of(half)).bytes, half.size());
    for (int i = 0; i < 20; ++i) step(kTestNow + 1);  // it arrives at "+1"
    EXPECT_EQ(server.connections(), 1u);
    step(kTestNow + 11);  // ten seconds on: still within the limit
    EXPECT_EQ(server.connections(), 1u);
    step(kTestNow + 12);
    EXPECT_EQ(server.connections(), 0u);
}

TEST(JsonRpc, CallsMethodsAndReportsErrors) {
    RpcTable t;
    t.add("add", "<a> <b>", "Adds.", 2, 2, [](const RpcParams& p) -> json { return p.integer(0) + p.integer(1); });
    t.add("fail", "", "Fails.", 0, 0, [](const RpcParams&) -> json { throw std::runtime_error("boom"); });
    t.add("coded", "", "Fails its own way.", 0, 0, [](const RpcParams&) -> json { throw RpcError(-42, "nope"); });
    t.add("amount", "<a>", "Reads an amount.", 1, 1, [](const RpcParams& p) -> json { return p.amount(0); });
    const auto reply = [&](const json& request) { return json::parse(t.handle(request.dump())); };

    const json ok = reply({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "add"}, {"params", {2, 3}}});
    EXPECT_EQ(ok["result"], 5);
    EXPECT_EQ(ok["id"], 7);
    EXPECT_FALSE(ok.contains("error"));
    EXPECT_EQ(reply({{"id", "x"}, {"method", "nope"}})["error"]["code"], rpc_error::kMethodNotFound);
    EXPECT_EQ(reply({{"id", "x"}, {"method", "nope"}})["id"], "x");
    EXPECT_EQ(reply({{"id", 1}, {"method", "add"}, {"params", {1}}})["error"]["code"], rpc_error::kInvalidParams);
    EXPECT_EQ(reply({{"id", 1}, {"method", "add"}, {"params", {1, "x"}}})["error"]["code"], rpc_error::kType);
    EXPECT_EQ(reply({{"id", 1}, {"method", "add"}, {"params", {{"a", 1}}}})["error"]["code"], rpc_error::kInvalidParams);
    const json failed = reply({{"id", 1}, {"method", "fail"}});
    EXPECT_EQ(failed["error"]["code"], rpc_error::kMisc);
    EXPECT_EQ(failed["error"]["message"], "boom");
    EXPECT_EQ(reply({{"id", 1}, {"method", "coded"}})["error"]["code"], -42);
    EXPECT_EQ(json::parse(t.handle("{not json"))["error"]["code"], rpc_error::kParse);
    EXPECT_EQ(json::parse(t.handle("[]"))["error"]["code"], rpc_error::kInvalidRequest);
    EXPECT_EQ(json::parse(t.handle("42"))["error"]["code"], rpc_error::kInvalidRequest);

    const json batch = json::parse(t.handle(R"([{"id":1,"method":"add","params":[1,1]},{"id":2,"method":"nope"}])"));
    ASSERT_EQ(batch.size(), 2u);
    EXPECT_EQ(batch[0]["result"], 2);
    EXPECT_EQ(batch[1]["error"]["code"], rpc_error::kMethodNotFound);

    // Amounts: exact strings, or numbers with at most 8 decimals; positive.
    const auto amount = [&](const json& a) { return reply({{"id", 1}, {"method", "amount"}, {"params", json::array({a})}}); };
    EXPECT_EQ(amount("1.5")["result"], 150'000'000);
    EXPECT_EQ(amount(1.5)["result"], 150'000'000);
    EXPECT_EQ(amount(0.1)["result"], 10'000'000);
    EXPECT_EQ(amount(2)["result"], 200'000'000);
    EXPECT_EQ(amount("0.00000001")["result"], 1);
    for (const json& bad : {json("0"), json(0), json(-1), json(0.123456789), json("1e3"), json(true), json(22'000'000)}) {
        EXPECT_EQ(amount(bad)["error"]["code"], rpc_error::kType) << bad.dump();
    }

    EXPECT_NE(t.help().find("add <a> <b>\n"), std::string::npos);
    EXPECT_EQ(t.help("add"), "add <a> <b>\n\nAdds.\n");
}

TEST(JsonRpc, OnlyWithTheCookie) {
    RpcTable t;
    t.add("ping", "", "", 0, 0, [](const RpcParams&) -> json { return "pong"; });
    const std::string cookie = make_cookie();
    EXPECT_TRUE(cookie.starts_with("__cookie__:"));
    EXPECT_EQ(cookie.size(), 11u + 64);
    EXPECT_NE(make_cookie(), cookie);

    HttpRequest request;
    request.method = "POST";
    request.target = "/";
    request.body = R"({"id":1,"method":"ping"})";
    EXPECT_EQ(serve_json_rpc(t, cookie, request).status, 401);
    request.headers["authorization"] = "Basic " + base64_encode(bytes_of("__cookie__:wrong"));
    EXPECT_EQ(serve_json_rpc(t, cookie, request).status, 401);
    request.headers["authorization"] = "Basic " + base64_encode(bytes_of(cookie));
    const HttpResponse ok = serve_json_rpc(t, cookie, request);
    EXPECT_EQ(ok.status, 200);
    EXPECT_EQ(json::parse(ok.body)["result"], "pong");
    request.method = "GET";
    EXPECT_EQ(serve_json_rpc(t, cookie, request).status, 405);
    request.method = "POST";
    request.target = "/wallet";
    EXPECT_EQ(serve_json_rpc(t, cookie, request).status, 404);
}

TEST(JsonRpc, ServedOverLoopback) {
    RpcTable t;
    t.add("echo", "<text>", "", 1, 1, [](const RpcParams& p) -> json { return p.str(0); });
    const std::string cookie = make_cookie();
    std::optional<HttpServer> server(std::in_place, std::uint16_t{0},
                                     [&](const HttpRequest& r) { return serve_json_rpc(t, cookie, r); });
    const NetAddress at = NetAddress::ipv4(127, 0, 0, 1, server->port());
    std::atomic<bool> stop{false};
    std::thread loop([&] {
        while (!stop) {
            std::vector<PollItem> items;
            server->add_poll_items(items);
            poll(items, 20);
            server->handle_poll(items, kTestNow);
        }
    });

    const std::vector<std::pair<std::string, std::string>> auth{{"Authorization", "Basic " + base64_encode(bytes_of(cookie))}};
    for (int i = 0; i < 5; ++i) {
        const HttpResponse ok = http_post(at, "/", R"({"jsonrpc":"2.0","id":3,"method":"echo","params":["hello"]})", auth, 5000);
        EXPECT_EQ(ok.status, 200);
        EXPECT_EQ(json::parse(ok.body)["result"], "hello");
    }
    EXPECT_EQ(http_post(at, "/", "{}", {}, 5000).status, 401);
    stop = true;
    loop.join();
    server.reset();
    EXPECT_THROW(http_post(at, "/", "{}", {}, 5000), HttpConnectError);
}

namespace {

// A node without sockets, driven through its RPC methods.
class NodeRpcTest : public ::testing::Test {
protected:
    NodeRpcTest() {
        node.wallet_options.kdf = kTestKdf;
        register_node_methods(table, node);
    }

    json call(const std::string& method, json args = json::array()) {
        const json reply = table.call(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", args}});
        if (reply.contains("error")) throw RpcError(reply["error"]["code"].get<int>(), reply["error"]["message"].get<std::string>());
        return reply["result"];
    }
    int error_code(const std::string& method, json args = json::array()) {
        try {
            call(method, std::move(args));
            return 0;
        } catch (const RpcError& e) {
            return e.code;
        }
    }

    const ConsensusParams params = easy_params();
    Chainstate chain{params};
    Mempool mempool{chain};
    AddrMan addrman{1};
    BanMan banman;
    PeerManager peers{chain, mempool, addrman, banman, {}, 1};
    MemoryStore wallet_db;
    NodeContext node{.chain = chain, .mempool = mempool, .peers = peers, .clock = [] { return kTestNow; }, .wallet_db = &wallet_db};
    RpcTable table;
};

}  // namespace

// Milestone 8: what an explorer asks, answered from the index.
TEST_F(NodeRpcTest, IndexQueries) {
    EXPECT_EQ(call("getindexinfo")["enabled"], false);
    call("createwallet", {"secret"});
    const std::string mine_to = call("getnewaddress");
    call("generatetoaddress", {101, mine_to});
    const std::string bob = encode_address(test_key("bob").public_key(), params.address_version);
    EXPECT_EQ(error_code("getaddressbalance", {bob}), rpc_error::kMisc);  // no index yet

    MemoryStore index_db;
    ChainIndex index(index_db, chain);
    node.index = &index;
    EXPECT_EQ(call("getindexinfo")["synced"], true);
    EXPECT_EQ(call("getindexinfo")["height"], 101);

    call("walletpassphrase", {"secret", 60});
    const std::string txid = call("sendtoaddress", {bob, "1.25", 2, "hi"});  // 193 bytes at 2 per byte
    // Waiting in the mempool: what it spends comes from the UTXO set.
    json tx = call("getrawtransaction", {txid, 2});
    EXPECT_EQ(tx["confirmations"], 0);
    EXPECT_EQ(tx["fee"], "0.00000386");
    EXPECT_EQ(tx["fee_rate"], 2.0);
    ASSERT_EQ(tx["vin"].size(), 1u);
    EXPECT_EQ(tx["vin"][0]["prevout"]["address"], mine_to);
    EXPECT_EQ(tx["vin"][0]["prevout"]["amount"], "5.00000000");
    EXPECT_EQ(tx["vin"][0]["prevout"]["coinbase"], true);
    const std::string spent_txid = tx["vin"][0]["txid"];
    EXPECT_EQ(call("getrawtransaction", {txid, true})["txid"], txid);  // true, as older bitcoind takes it
    EXPECT_TRUE(call("getrawtransaction", {txid}).is_string());
    EXPECT_EQ(error_code("getrawtransaction", {txid, 3}), rpc_error::kType);
    EXPECT_TRUE(call("gettxout", {spent_txid, 0}).is_null());  // a waiting transaction spends it
    EXPECT_EQ(call("gettxout", {spent_txid, 0, false})["amount"], "5.00000000");

    call("generatetoaddress", {1, mine_to});
    tx = call("getrawtransaction", {txid, 2});
    EXPECT_EQ(tx["confirmations"], 1);
    EXPECT_EQ(tx["blockheight"], 102);
    EXPECT_EQ(tx["position"], 1);
    EXPECT_EQ(tx["fee"], "0.00000386");
    EXPECT_EQ(tx["vin"][0]["prevout"]["height"], 1);  // now from the block's undo data
    const std::size_t bob_n = tx["vout"][0]["address"] == bob ? 0 : 1;
    EXPECT_EQ(tx["vout"][bob_n]["spent"], false);
    EXPECT_EQ(call("gettxout", {txid, bob_n})["amount"], "1.25000000");
    EXPECT_EQ(call("gettxout", {txid, bob_n})["confirmations"], 1);
    EXPECT_TRUE(call("gettxout", {txid, 7}).is_null());
    EXPECT_TRUE(call("gettxout", {spent_txid, 0, false}).is_null());  // really spent now

    // The coinbase it spent is neither in the mempool nor (as a lookup) needed from the wallet:
    // the index finds it, and says who spent it.
    const json coinbase = call("getrawtransaction", {spent_txid, 2});
    EXPECT_EQ(coinbase["coinbase"], true);
    EXPECT_EQ(coinbase["blockheight"], 1);
    EXPECT_EQ(coinbase["vout"][0]["spent"], true);
    EXPECT_EQ(coinbase["vout"][0]["spent_by"], txid);
    EXPECT_FALSE(coinbase.contains("fee"));

    // A node with no wallet only finds confirmed transactions through the index.
    NodeContext bare{.chain = chain, .mempool = mempool, .peers = peers, .clock = [] { return kTestNow; }};
    RpcTable bare_table;
    register_node_methods(bare_table, bare);
    const json request = {{"id", 1}, {"method", "getrawtransaction"}, {"params", {txid, 1}}};
    EXPECT_EQ(bare_table.call(request)["error"]["code"], rpc_error::kInvalidAddressOrKey);
    bare.index = &index;
    EXPECT_EQ(bare_table.call(request)["result"]["blockheight"], 102);

    const json balance = call("getaddressbalance", {bob});
    EXPECT_EQ(balance["balance"], "1.25000000");
    EXPECT_EQ(balance["txcount"], 1);
    EXPECT_EQ(balance["utxos"], 1);
    const json deltas = call("getaddressdeltas", {mine_to, 2});
    ASSERT_EQ(deltas.size(), 2u);
    EXPECT_EQ(deltas[0]["txid"], txid);  // newest first: the payment, after the coinbase in its block
    EXPECT_EQ(deltas[0]["spent"], "5.00000000");
    EXPECT_EQ(deltas[0]["received"], "0.00000000");  // the change went to one of the wallet's change addresses
    EXPECT_EQ(deltas[0]["delta"], "-5.00000000");
    EXPECT_EQ(deltas[1]["height"], 102);
    EXPECT_EQ(deltas[1]["position"], 0);
    EXPECT_EQ(call("getaddressdeltas", {mine_to, 2, 1})[0], deltas[1]);
    const json utxos = call("getaddressutxos", {bob});
    ASSERT_EQ(utxos.size(), 1u);
    EXPECT_EQ(utxos[0]["txid"], txid);
    EXPECT_EQ(utxos[0]["amount"], "1.25000000");
    EXPECT_EQ(utxos[0]["confirmations"], 1);
    EXPECT_EQ(error_code("getaddressdeltas", {bob, 0}), rpc_error::kInvalidParameter);
    EXPECT_EQ(error_code("getaddressbalance", {"nonsense"}), rpc_error::kInvalidAddressOrKey);

    const json block = call("getblock", {call("getbestblockhash"), 2});
    EXPECT_EQ(block["tx"][1]["fee"], "0.00000386");
    EXPECT_FALSE(block["tx"][0].contains("fee"));
    EXPECT_GT(call("getnetworkhashps").get<double>(), 0.0);
    node.index = nullptr;
}

TEST_F(NodeRpcTest, Bans) {
    EXPECT_EQ(error_code("listbanned"), rpc_error::kMisc);  // no BanMan given
    node.banman = &banman;
    int saves = 0;
    node.save_bans = [&] { ++saves; };

    EXPECT_TRUE(call("setban", {"10.1.2.3", "add", 600}).is_null());
    EXPECT_EQ(saves, 1);
    EXPECT_TRUE(banman.is_banned(NetAddress::ipv4(10, 1, 2, 3, 18333), kTestNow));
    const json list = call("listbanned");
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0]["address"], "10.1.2.3");
    EXPECT_EQ(list[0]["ban_remaining"], 600);
    call("setban", {"[2001:db8::1]", "add"});
    EXPECT_EQ(call("listbanned").size(), 2u);

    call("setban", {"10.1.2.3", "remove"});
    EXPECT_EQ(error_code("setban", {"10.1.2.3", "remove"}), rpc_error::kMisc);
    EXPECT_EQ(error_code("setban", {"not an address!", "add"}), rpc_error::kInvalidParameter);
    EXPECT_EQ(error_code("setban", {"10.1.2.3", "maybe"}), rpc_error::kInvalidParameter);
    EXPECT_EQ(error_code("setban", {"10.1.2.3", "add", -5}), rpc_error::kInvalidParameter);
    call("clearbanned");
    EXPECT_TRUE(call("listbanned").empty());
    EXPECT_EQ(saves, 4);
}

TEST_F(NodeRpcTest, AWholeRoundTrip) {
    EXPECT_EQ(call("getblockcount"), 0);
    EXPECT_EQ(error_code("getbalance"), rpc_error::kNoWallet);
    EXPECT_EQ(error_code("getblockcount", {1}), rpc_error::kInvalidParams);
    const json created = call("createwallet", {"secret"});
    const std::string seed = created["seed"];
    const std::string words = created["words"];  // milestone 17: the same seed, as people write it down
    EXPECT_EQ(seed.size(), 64u);
    EXPECT_EQ(std::count(words.begin(), words.end(), ' '), 23);
    EXPECT_EQ(parse_seed(words), parse_seed(seed));
    EXPECT_EQ(error_code("createwallet", {"again"}), rpc_error::kWallet);

    const std::string mine_to = call("getnewaddress");
    EXPECT_EQ(call("validateaddress", {mine_to})["ismine"], true);
    EXPECT_EQ(call("validateaddress", {"nonsense"})["isvalid"], false);
    const json hashes = call("generatetoaddress", {101, mine_to});
    ASSERT_EQ(hashes.size(), 101u);
    EXPECT_EQ(call("getblockcount"), 101);
    EXPECT_EQ(call("getbestblockhash"), hashes.back());
    EXPECT_EQ(call("getblockhash", {101}), hashes.back());
    EXPECT_EQ(error_code("getblockhash", {500}), rpc_error::kInvalidParameter);
    EXPECT_EQ(call("getblock", {hashes[0], 2})["tx"][0]["coinbase"], true);
    // json::array: a braced list holding a single json would copy it rather than make an array.
    EXPECT_EQ(call("getblockheader", json::array({hashes[0]}))["confirmations"], 101);
    EXPECT_EQ(call("getbalance"), "10.00000000");  // the coinbases of #1 and #2 are spendable in #102
    EXPECT_EQ(call("getwalletinfo")["immature_balance"], "495.00000000");

    const std::string bob = encode_address(test_key("bob").public_key(), params.address_version);
    EXPECT_EQ(error_code("sendtoaddress", {bob, "1"}), rpc_error::kUnlockNeeded);
    EXPECT_EQ(error_code("walletpassphrase", {"wrong", 60}), rpc_error::kWrongPassphrase);
    EXPECT_TRUE(call("walletpassphrase", {"secret", 60}).is_null());
    EXPECT_EQ(error_code("sendtoaddress", {bob, "100"}), rpc_error::kInsufficientFunds);
    EXPECT_EQ(error_code("sendtoaddress", {"9nope", "1"}), rpc_error::kInvalidAddressOrKey);

    const std::string txid = call("sendtoaddress", {bob, "1.25", 2, "hi"});  // 193 bytes at 2 per byte
    EXPECT_EQ(call("getrawmempool"), json::array({txid}));
    EXPECT_EQ(call("getmempoolentry", {txid})["fee"], "0.00000386");
    EXPECT_EQ(call("getmempoolentry", {txid})["fee_rate"], 2.0);
    const json decoded = call("getrawtransaction", {txid, true});
    EXPECT_EQ(decoded["memo_text"], "hi");
    EXPECT_EQ(call("decoderawtransaction", json::array({decoded["hex"]}))["txid"], txid);
    EXPECT_EQ(call("getbalance"), "8.74999614");
    EXPECT_EQ(call("sendrawtransaction", json::array({decoded["hex"]})), txid);  // already there: fine
    EXPECT_EQ(error_code("sendrawtransaction", {"zz"}), rpc_error::kDeserialization);

    const json bumped = call("bumpfee", {txid, 10});
    EXPECT_EQ(bumped["origfee"], "0.00000386");
    EXPECT_EQ(bumped["fee"], "0.00001930");
    const std::string replacement = bumped["txid"];
    EXPECT_EQ(call("getrawmempool"), json::array({replacement}));

    call("generatetoaddress", {1, mine_to});
    EXPECT_EQ(call("getmempoolinfo")["size"], 0);
    const json sent = call("gettransaction", {replacement});
    EXPECT_EQ(sent["category"], "send");
    EXPECT_EQ(sent["amount"], "-1.25000000");
    EXPECT_EQ(sent["fee"], "0.00001930");
    EXPECT_EQ(sent["confirmations"], 1);
    const json last = call("listtransactions", {3});
    ASSERT_EQ(last.size(), 3u);
    EXPECT_EQ(last[2]["category"], "immature");  // the coinbase of #102

    EXPECT_TRUE(call("walletlock").is_null());
    EXPECT_EQ(error_code("dumpseed"), rpc_error::kUnlockNeeded);
    call("walletpassphrase", {"secret", 60});
    EXPECT_EQ(call("dumpseed"), (json{{"words", words}, {"seed", seed}}));
    EXPECT_EQ(call("getblockchaininfo")["blocks"], 102);

    // The seed brings everything back in another wallet.
    MemoryStore other_db;
    NodeContext other{.chain = chain, .mempool = mempool, .peers = peers, .clock = [] { return kTestNow; }, .wallet_db = &other_db};
    other.wallet_options.kdf = kTestKdf;
    RpcTable other_table;
    register_node_methods(other_table, other);
    const json misspelt = other_table.call(json{{"id", 1}, {"method", "restorewallet"}, {"params", {"abandon ability abel", "other"}}});
    EXPECT_EQ(misspelt["error"]["code"], rpc_error::kInvalidParameter);
    EXPECT_EQ(misspelt["error"]["message"], "the seed: Word 3, \"abel\", isn't one of the seed words.");
    const json restored = other_table.call(json{{"id", 1}, {"method", "restorewallet"}, {"params", {words, "other"}}});
    EXPECT_EQ(restored["result"]["balance"], call("getbalance"));
    // 102 coinbases and the replacement; the replaced original never made it into the chain.
    EXPECT_EQ(restored["result"]["transactions"], 103);
    EXPECT_EQ(call("getwalletinfo")["txcount"], 104);
}

// Milestone 9: mining in the background, switched by RPC (the node's loop does the mining and
// reports back through MiningControl).
TEST_F(NodeRpcTest, MiningSwitchedOnAndOff) {
    EXPECT_EQ(call("getmininginfo")["mining"], false);
    EXPECT_EQ(call("getmininginfo")["status"], "off");
    EXPECT_EQ(error_code("setmining", json::array({true})), rpc_error::kMisc);  // not available here

    MiningControl mining;
    node.mining = &mining;
    EXPECT_EQ(error_code("setmining", json::array({true})), rpc_error::kNoWallet);  // nobody to pay
    EXPECT_EQ(error_code("setmining", json::array({true, "9notanaddress"})), rpc_error::kInvalidAddressOrKey);
    EXPECT_EQ(error_code("setmining", json::array({"yes"})), rpc_error::kType);
    EXPECT_FALSE(mining.payee.has_value());

    const std::string carol = encode_address(test_key("carol").public_key(), params.address_version);
    const json on = call("setmining", json::array({true, carol}));
    EXPECT_EQ(on["mining"], true);
    EXPECT_EQ(on["status"], "mining");
    EXPECT_EQ(on["address"], carol);
    EXPECT_EQ(mining.payee, test_key("carol").public_key());

    mining.paused = true;
    mining.hashes_per_second = 1500;
    mining.blocks_found = 2;
    const json info = call("getmininginfo");
    EXPECT_EQ(info["status"], "waiting for the initial sync");
    EXPECT_EQ(info["hashespersec"], 1500.0);
    EXPECT_EQ(info["blocksfound"], 2);
    EXPECT_EQ(info["blocks"], 0);

    EXPECT_EQ(call("setmining", json::array({false}))["mining"], false);
    EXPECT_FALSE(mining.payee.has_value());

    // With a wallet and no address, a new address of the wallet's.
    call("createwallet", {"secret"});
    const json mine = call("setmining", json::array({true}));
    // json::array: a lone json value in braces is a copy of it on MSVC, not a one-element list.
    EXPECT_EQ(call("validateaddress", json::array({mine["address"]}))["ismine"], true);
}

// Milestone 18: the wallet app's Max button.
TEST_F(NodeRpcTest, SendAllEmptiesTheWallet) {
    call("createwallet", {"secret"});
    const std::string mine_to = call("getnewaddress");
    call("generatetoaddress", {101, mine_to});
    EXPECT_EQ(call("getbalance"), "10.00000000");
    const std::string bob = encode_address(test_key("bob").public_key(), params.address_version);
    EXPECT_EQ(error_code("sendall", {bob}), rpc_error::kUnlockNeeded);
    call("walletpassphrase", {"secret", 60});
    const json sent = call("sendall", {bob, 1});
    EXPECT_EQ(sent["amount"], "9.99999749");  // 2 inputs and 1 output: 251 bytes at 1 unit a byte
    EXPECT_EQ(sent["fee"], "0.00000251");
    EXPECT_EQ(call("getrawmempool"), json::array({sent["txid"]}));
    EXPECT_EQ(call("getbalance"), "0.00000000");
    EXPECT_EQ(error_code("sendall", {bob}), rpc_error::kInsufficientFunds);
}

// Milestone 9, for the wallet app: listtransactions says who each payment went to or came from.
TEST_F(NodeRpcTest, TransactionsShowTheirAddress) {
    call("createwallet", {"secret"});
    const std::string mine_to = call("getnewaddress");
    call("generatetoaddress", {101, mine_to});
    const json coinbase = call("listtransactions", {1})[0];
    EXPECT_EQ(coinbase["category"], "immature");
    EXPECT_EQ(coinbase["address"], mine_to);

    const std::string bob = encode_address(test_key("bob").public_key(), params.address_version);
    call("walletpassphrase", {"secret", 60});
    const std::string txid = call("sendtoaddress", {bob, "1.5"});
    const json sent = call("listtransactions", {1})[0];
    EXPECT_EQ(sent["txid"], txid);
    EXPECT_EQ(sent["category"], "send");
    EXPECT_EQ(sent["address"], bob);  // not the change, which comes back to us
    EXPECT_EQ(call("gettransaction", json::array({txid}))["address"], bob);
}