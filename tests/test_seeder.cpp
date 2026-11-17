#include "core/validation.h"
#include "net/addrman.h"
#include "net/banman.h"
#include "net/connman.h"
#include "net/peerman.h"
#include "net/socket.h"
#include "seeder/crawler.h"
#include "seeder/dns.h"
#include "seeder/dns_server.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace bound;
using namespace std::chrono_literals;

namespace {

void put16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// A query as a resolver sends it: the name's letters in whatever case it likes.
Bytes query(std::uint16_t id, std::string_view name, std::uint16_t type, std::uint16_t edns_size = 0, std::uint16_t flags = 0x0100,
            std::uint16_t klass = dns::kClassIN) {
    Bytes q;
    put16(q, id);
    put16(q, flags);
    put16(q, 1);
    put16(q, 0);
    put16(q, 0);
    put16(q, edns_size != 0 ? 1 : 0);
    while (!name.empty()) {
        const std::size_t dot = name.find('.');
        const std::string_view label = name.substr(0, dot);
        q.push_back(static_cast<std::uint8_t>(label.size()));
        q.insert(q.end(), label.begin(), label.end());
        if (dot == std::string_view::npos) break;
        name.remove_prefix(dot + 1);
    }
    q.push_back(0);
    put16(q, type);
    put16(q, klass);
    if (edns_size != 0) {
        q.push_back(0);
        put16(q, dns::kTypeOPT);
        put16(q, edns_size);
        q.insert(q.end(), {0, 0, 0, 0, 0, 0});
    }
    return q;
}

struct Record {
    std::uint16_t type = 0;
    std::uint32_t ttl = 0;
    Bytes rdata;
};

struct Reply {
    std::uint16_t id = 0;
    std::uint16_t flags = 0;
    std::uint16_t questions = 0;
    std::vector<Record> answers;
    std::vector<Record> authority;
    std::uint16_t additional = 0;
    Bytes question_name;
    std::uint8_t rcode() const { return static_cast<std::uint8_t>(flags & 0xf); }
};

// Reads a reply the way a resolver would, enough to check one. (A copy: span has no bounds-checked at() yet.)
Reply read_reply(ByteSpan packet) {
    const Bytes p(packet.begin(), packet.end());
    const auto u16 = [&](std::size_t at) { return static_cast<std::uint16_t>(p.at(at) << 8 | p.at(at + 1)); };
    Reply r;
    r.id = u16(0);
    r.flags = u16(2);
    r.questions = u16(4);
    const std::uint16_t an = u16(6);
    const std::uint16_t ns = u16(8);
    r.additional = u16(10);
    std::size_t at = 12;
    const auto skip_name = [&] {
        while (true) {
            const std::uint8_t length = p.at(at);
            if (length == 0) {
                ++at;
                return;
            }
            if ((length & 0xc0) == 0xc0) {
                at += 2;
                return;
            }
            at += 1 + length;
        }
    };
    if (r.questions == 1) {
        const std::size_t start = at;
        skip_name();
        r.question_name.assign(p.begin() + static_cast<std::ptrdiff_t>(start), p.begin() + static_cast<std::ptrdiff_t>(at));
        at += 4;
    }
    for (int i = 0; i < an + ns; ++i) {
        skip_name();
        Record rec;
        rec.type = u16(at);
        rec.ttl = static_cast<std::uint32_t>(u16(at + 4)) << 16 | u16(at + 6);
        const std::uint16_t length = u16(at + 8);
        at += 10;
        rec.rdata.assign(p.begin() + static_cast<std::ptrdiff_t>(at), p.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        (i < an ? r.answers : r.authority).push_back(std::move(rec));
    }
    return r;
}

DnsZone zone() {
    DnsZone z;
    z.host = "seed.example.org";
    z.nameserver = "ns.example.org";
    z.mailbox = "hostmaster.example.org";
    z.serial = 7;
    return z;
}

Reply ask(const Bytes& q, std::span<const NetAddress> addresses) {
    const std::optional<DnsQuery> parsed = parse_dns_query(q);
    if (!parsed) throw std::logic_error("the test's query didn't parse");
    return read_reply(build_dns_reply(*parsed, zone(), addresses));
}

std::vector<NetAddress> many(int ipv4, int ipv6) {
    std::vector<NetAddress> list;
    for (int i = 0; i < ipv4; ++i) list.push_back(NetAddress::ipv4(1, 2, 3, static_cast<std::uint8_t>(i), 18333));
    for (int i = 0; i < ipv6; ++i) {
        NetAddress a = resolve("[2606:4700::1]", 18333).at(0);
        a.ip[15] = static_cast<std::uint8_t>(i);
        list.push_back(a);
    }
    return list;
}

}  // namespace

TEST(Dns, NormalizesNames) {
    EXPECT_EQ(normalize_dns_name("Seed.Example.ORG."), "seed.example.org");
    EXPECT_EQ(normalize_dns_name("a-b_c.d1"), "a-b_c.d1");
    EXPECT_EQ(normalize_dns_name(""), std::nullopt);
    EXPECT_EQ(normalize_dns_name("."), std::nullopt);
    EXPECT_EQ(normalize_dns_name("a..b"), std::nullopt);
    EXPECT_EQ(normalize_dns_name("has space.org"), std::nullopt);
    EXPECT_EQ(normalize_dns_name(std::string(64, 'a') + ".org"), std::nullopt);
    EXPECT_TRUE(normalize_dns_name(std::string(63, 'a') + ".org").has_value());
}

TEST(Dns, ParsesQueries) {
    const std::optional<DnsQuery> q = parse_dns_query(query(0x1234, "SeEd.Example.ORG", dns::kTypeA, 4096));
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->id, 0x1234);
    EXPECT_EQ(q->name, "seed.example.org");
    EXPECT_EQ(q->type, dns::kTypeA);
    EXPECT_EQ(q->klass, dns::kClassIN);
    EXPECT_TRUE(q->recursion_desired);
    EXPECT_TRUE(q->edns);
    EXPECT_EQ(q->udp_size, 4096);
    EXPECT_EQ(q->raw_name[1], 'S');  // the case as it came

    EXPECT_FALSE(parse_dns_query(query(1, "a.org", dns::kTypeA, 0, 0x8000)).has_value());  // a reply
    Bytes two = query(1, "a.org", dns::kTypeA);
    two[5] = 2;
    EXPECT_FALSE(parse_dns_query(two).has_value());
    Bytes pointer = query(1, "a.org", dns::kTypeA);
    pointer[12] = 0xc0;
    EXPECT_FALSE(parse_dns_query(pointer).has_value());
    const Bytes whole = query(1, "a.org", dns::kTypeA);
    for (std::size_t n = 0; n < whole.size(); ++n) {
        EXPECT_FALSE(parse_dns_query(ByteSpan(whole).first(n)).has_value()) << n;
    }
    EXPECT_FALSE(parse_dns_query(query(1, std::string(64, 'x') + ".org", dns::kTypeA)).has_value());
}

TEST(Dns, AnswersWithTheNodesThatFit) {
    const std::vector<NetAddress> nodes = many(30, 3);
    Reply r = ask(query(77, "SeEd.Example.ORG", dns::kTypeA), nodes);
    EXPECT_EQ(r.id, 77);
    EXPECT_EQ(r.flags & 0x8000, 0x8000);  // a reply
    EXPECT_EQ(r.flags & 0x0400, 0x0400);  // authoritative
    EXPECT_EQ(r.flags & 0x0100, 0x0100);  // recursion desired, copied
    EXPECT_EQ(r.flags & 0x0200, 0);       // never "truncated"
    EXPECT_EQ(r.rcode(), dns::kNoError);
    EXPECT_EQ(r.question_name, (Bytes{4, 'S', 'e', 'E', 'd', 7, 'E', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'O', 'R', 'G', 0}));
    ASSERT_EQ(r.answers.size(), 29u);  // 512 bytes hold 29 of the 30
    EXPECT_EQ(r.answers[0].type, dns::kTypeA);
    EXPECT_EQ(r.answers[0].ttl, 300u);
    EXPECT_EQ(r.answers[0].rdata, (Bytes{1, 2, 3, 0}));
    EXPECT_LE(build_dns_reply(*parse_dns_query(query(77, "seed.example.org", dns::kTypeA)), zone(), nodes).size(), 512u);

    r = ask(query(78, "seed.example.org", dns::kTypeA, 1232), nodes);  // a client that takes more
    EXPECT_EQ(r.answers.size(), 30u);
    EXPECT_EQ(r.additional, 1);  // our OPT record

    r = ask(query(79, "seed.example.org", dns::kTypeAAAA), nodes);
    ASSERT_EQ(r.answers.size(), 3u);
    EXPECT_EQ(r.answers[0].type, dns::kTypeAAAA);
    EXPECT_EQ(r.answers[0].rdata.size(), 16u);

    r = ask(query(80, "seed.example.org", dns::kTypeAAAA), many(5, 0));  // no IPv6 nodes: "no data"
    EXPECT_EQ(r.rcode(), dns::kNoError);
    EXPECT_TRUE(r.answers.empty());
    ASSERT_EQ(r.authority.size(), 1u);
    EXPECT_EQ(r.authority[0].type, dns::kTypeSOA);
}

TEST(Dns, AnswersOnlyForItsZone) {
    const std::vector<NetAddress> nodes = many(3, 0);
    Reply r = ask(query(1, "other.seed.example.org", dns::kTypeA), nodes);
    EXPECT_EQ(r.rcode(), dns::kNXDomain);
    EXPECT_TRUE(r.answers.empty());
    EXPECT_EQ(r.authority.size(), 1u);
    EXPECT_EQ(ask(query(2, "example.org", dns::kTypeA), nodes).rcode(), dns::kRefused);
    EXPECT_EQ(ask(query(3, "google.com", dns::kTypeA), nodes).rcode(), dns::kRefused);
    EXPECT_EQ(ask(query(4, "seed.example.org", dns::kTypeA, 0, 0x0900), nodes).rcode(), dns::kNotImp);  // opcode 1
    EXPECT_EQ(ask(query(5, "seed.example.org", dns::kTypeA, 0, 0x0100, 3), nodes).rcode(), dns::kRefused);  // class CH

    r = ask(query(6, "seed.example.org", dns::kTypeNS), nodes);
    ASSERT_EQ(r.answers.size(), 1u);
    EXPECT_EQ(r.answers[0].type, dns::kTypeNS);
    EXPECT_EQ(r.answers[0].rdata, (Bytes{2, 'n', 's', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'o', 'r', 'g', 0}));
    r = ask(query(7, "seed.example.org", dns::kTypeSOA), nodes);
    ASSERT_EQ(r.answers.size(), 1u);
    EXPECT_EQ(r.answers[0].type, dns::kTypeSOA);
    r = ask(query(8, "seed.example.org", 16), nodes);  // TXT: the name exists, with nothing of that type
    EXPECT_EQ(r.rcode(), dns::kNoError);
    EXPECT_TRUE(r.answers.empty());
    EXPECT_EQ(r.authority.size(), 1u);
    r = ask(query(9, "seed.example.org", dns::kTypeANY), nodes);
    EXPECT_EQ(r.answers.size(), 3u);

    const Reply error = read_reply(build_dns_error(0xbeef, dns::kFormErr));
    EXPECT_EQ(error.id, 0xbeef);
    EXPECT_EQ(error.rcode(), dns::kFormErr);
    EXPECT_EQ(error.questions, 0);
}

// The whole seeder on loopback: the crawler finds a real node and what it knows, and the DNS
// server hands the node out.
TEST(Seeder, CrawlsANodeAndServesIt) {
    const ConsensusParams params = easy_params();
    Chainstate chain(params);
    Mempool mempool(chain);
    mine_blocks(chain, test_key("alice").public_key(), 12);
    AddrMan addrs(1);
    const NetAddress elsewhere = NetAddress::ipv4(127, 0, 0, 2, 18444);  // a node it knows; nothing listens there
    addrs.add(elsewhere, kTestNow);
    BanMan bans;
    PeerManager peers(chain, mempool, addrs, bans, {}, 1);
    ConnmanOptions listening;
    listening.loopback_only = true;
    Connman net(peers, addrs, bans, listening, {}, [] { return kTestNow; });
    const NetAddress node = NetAddress::ipv4(127, 0, 0, 1, net.port());
    std::thread thread([&] { net.run(); });

    CrawlerOptions options;
    options.magic = params.message_start;
    options.default_port = net.port();
    options.allow_private = true;
    options.connect_timeout = 3;
    Crawler crawler(options, 5);
    ASSERT_TRUE(crawler.add(node, kTestNow));
    EXPECT_FALSE(crawler.add(node, kTestNow));
    const auto step = [&] {
        crawler.tick(kTestNow);
        std::vector<PollItem> items;
        crawler.add_poll_items(items);
        poll(items, 20);
        crawler.handle_poll(items, kTestNow);
    };
    for (int i = 0; i < 250 && crawler.good(kTestNow).empty(); ++i) step();
    net.stop();
    thread.join();

    ASSERT_EQ(crawler.good(kTestNow), std::vector<NetAddress>{node});
    const NodeRecord* record = crawler.find(node);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->height, 12);
    EXPECT_EQ(record->user_agent, user_agent("boundd"));
    EXPECT_EQ(record->successes, 1u);
    EXPECT_NE(crawler.find(elsewhere), nullptr);  // learned from its getaddr reply

    // seeder.dat keeps it all.
    Crawler reloaded(options, 6);
    reloaded.load(crawler.serialize());
    EXPECT_EQ(*reloaded.find(node), *record);
    EXPECT_EQ(reloaded.size(), crawler.size());

    // An A record has no port: a node that isn't on the network's port is useless to hand out.
    CrawlerOptions other_port = options;
    other_port.default_port = static_cast<std::uint16_t>(net.port() + 1);
    Crawler elsewhere_port(other_port, 8);
    elsewhere_port.load(crawler.serialize());
    EXPECT_EQ(elsewhere_port.find(node)->successes, 1u);
    EXPECT_TRUE(elsewhere_port.good(kTestNow).empty());

    // The DNS side, over UDP.
    DnsServer server(UdpSocket::bind(0, NetAddress::ipv4(127, 0, 0, 1, 0)), zone(), [&] { return crawler.good(kTestNow); }, 7);
    const UdpSocket client = UdpSocket::bind(0, NetAddress::ipv4(127, 0, 0, 1, 0));
    const NetAddress server_addr = NetAddress::ipv4(127, 0, 0, 1, server.port());
    const auto serve = [&] {
        std::vector<PollItem> items;
        server.add_poll_items(items);
        poll(items, 200);
        server.handle_poll(items, kTestNow);
    };
    ASSERT_TRUE(client.send(query(42, "seed.example.org", dns::kTypeA), server_addr));
    serve();
    std::array<std::uint8_t, 1500> buffer;
    std::optional<std::pair<std::size_t, NetAddress>> got;
    for (int i = 0; i < 100 && !got; ++i) {
        std::array<PollItem, 1> wait{PollItem{client.handle(), true, false}};
        poll(wait, 20);
        got = client.receive(buffer);
    }
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->second, server_addr);
    const Reply r = read_reply(ByteSpan(buffer.data(), got->first));
    EXPECT_EQ(r.id, 42);
    ASSERT_EQ(r.answers.size(), 1u);
    EXPECT_EQ(r.answers[0].rdata, (Bytes{127, 0, 0, 1}));
    EXPECT_EQ(server.stats().queries, 1u);

    // One address asking too often: 20 in a burst are answered (counting the one above), then no more.
    for (int i = 0; i < 30; ++i) ASSERT_TRUE(client.send(query(static_cast<std::uint16_t>(100 + i), "seed.example.org", dns::kTypeA), server_addr));
    for (int i = 0; i < 20 && server.stats().queries + server.stats().dropped < 31; ++i) serve();
    EXPECT_EQ(server.stats().queries, 20u);
    EXPECT_EQ(server.stats().dropped, 11u);
}

TEST(Seeder, KeepsOnlyUsableAddresses) {
    CrawlerOptions options;
    options.default_port = 18333;
    Crawler crawler(options, 1);
    EXPECT_FALSE(crawler.add(NetAddress::ipv4(127, 0, 0, 1, 18333), kTestNow));  // local: not on a public seeder
    EXPECT_FALSE(crawler.add(NetAddress::ipv4(192, 168, 1, 5, 18333), kTestNow));
    EXPECT_FALSE(crawler.add(NetAddress::ipv4(8, 8, 8, 8, 0), kTestNow));
    EXPECT_TRUE(crawler.add(NetAddress::ipv4(8, 8, 8, 8, 18333), kTestNow));
    EXPECT_TRUE(crawler.good(kTestNow).empty());  // known, but never answered
    EXPECT_THROW(crawler.load(Bytes{9}), DeserializeError);
    EXPECT_THROW(crawler.load(Bytes{1, 200}), DeserializeError);
}

// Milestone 14: a public seeder can start from the node beside it on 127.0.0.1 (--node), which
// leads it to the network, but a local address is never what it hands out, nor is any private
// address that node tells it about.
TEST(Seeder, StartsFromItsOwnNodeButNeverHandsItOut) {
    const ConsensusParams params = easy_params();
    Chainstate chain(params);
    Mempool mempool(chain);
    mine_blocks(chain, test_key("alice").public_key(), 3);
    AddrMan addrs(1);
    const NetAddress lan_peer = NetAddress::ipv4(192, 168, 1, 7, 18444);
    addrs.add(lan_peer, kTestNow);
    BanMan bans;
    PeerManager peers(chain, mempool, addrs, bans, {}, 1);
    ConnmanOptions listening;
    listening.loopback_only = true;
    Connman net(peers, addrs, bans, listening, {}, [] { return kTestNow; });
    const NetAddress node = NetAddress::ipv4(127, 0, 0, 1, net.port());
    std::thread thread([&] { net.run(); });

    CrawlerOptions options;
    options.magic = params.message_start;
    options.default_port = net.port();
    options.connect_timeout = 3;
    Crawler crawler(options, 5);
    EXPECT_FALSE(crawler.add(node, kTestNow));  // from gossip, a local address is refused
    ASSERT_TRUE(crawler.add_start(node, kTestNow));
    EXPECT_FALSE(crawler.add_start(node, kTestNow));
    EXPECT_FALSE(crawler.add_start(NetAddress::ipv4(127, 0, 0, 1, 0), kTestNow));
    const auto answered = [&] {
        const NodeRecord* r = crawler.find(node);
        return r != nullptr && r->successes > 0;
    };
    for (int i = 0; i < 250 && !answered(); ++i) {
        crawler.tick(kTestNow);
        std::vector<PollItem> items;
        crawler.add_poll_items(items);
        poll(items, 20);
        crawler.handle_poll(items, kTestNow);
    }
    net.stop();
    thread.join();

    ASSERT_TRUE(answered());
    EXPECT_EQ(crawler.find(node)->height, 3);
    EXPECT_TRUE(crawler.good(kTestNow).empty());
    EXPECT_EQ(crawler.find(lan_peer), nullptr);
}
