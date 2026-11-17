#include "core/validation.h"
#include "net/addrman.h"
#include "net/connman.h"
#include "net/peerman.h"
#include "net/socket.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace bound;
using namespace std::chrono_literals;

TEST(Sockets, SplitHostPort) {
    using Split = std::optional<std::pair<std::string, std::uint16_t>>;
    EXPECT_EQ(split_host_port("example.org", 18333), (Split{{"example.org", 18333}}));
    EXPECT_EQ(split_host_port("example.org:1234", 18333), (Split{{"example.org", 1234}}));
    EXPECT_EQ(split_host_port("[::1]:1234", 18333), (Split{{"::1", 1234}}));
    EXPECT_EQ(split_host_port("::1", 18333), (Split{{"::1", 18333}}));  // a bare IPv6 address
    EXPECT_EQ(split_host_port("host:0", 18333), std::nullopt);
    EXPECT_EQ(split_host_port("host:65536", 18333), std::nullopt);
    EXPECT_EQ(split_host_port("host:", 18333), std::nullopt);
    EXPECT_EQ(split_host_port(":1234", 18333), std::nullopt);
    EXPECT_EQ(split_host_port("[::1", 18333), std::nullopt);
}

TEST(Sockets, ResolvesLiteralAddresses) {
    const std::vector<NetAddress> v4 = resolve("127.0.0.1:5000", 1);
    ASSERT_EQ(v4.size(), 1u);
    EXPECT_EQ(v4[0], NetAddress::ipv4(127, 0, 0, 1, 5000));
    EXPECT_EQ(v4[0].to_string(), "127.0.0.1:5000");
    const std::vector<NetAddress> v6 = resolve("[::1]:7", 1);
    ASSERT_EQ(v6.size(), 1u);
    EXPECT_EQ(v6[0].to_string(), "[::1]:7");
    EXPECT_TRUE(resolve("not a host name", 1).empty());
}

TEST(Sockets, RoutableAddresses) {
    for (const char* text : {"8.8.8.8", "1.2.3.4", "172.15.0.1", "172.32.0.1", "100.63.0.1", "100.128.0.1", "193.0.0.1",
                             "198.17.0.1", "198.20.0.1", "[2606:4700::1111]", "[2001:4860:4860::8888]"}) {
        EXPECT_TRUE(resolve(text, 1).at(0).is_routable()) << text;
    }
    for (const char* text : {"10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.0.1", "127.0.0.1", "169.254.1.1",
                             "100.64.0.1", "100.127.0.1", "0.1.2.3", "224.0.0.1", "255.255.255.255", "192.0.2.5",
                             "198.51.100.5", "203.0.113.5", "198.18.0.1", "198.19.255.1", "[::1]", "[fe80::1]", "[fc00::1]",
                             "[fd12::1]", "[ff02::1]", "[2001:db8::1]"}) {
        EXPECT_FALSE(resolve(text, 1).at(0).is_routable()) << text;
    }
    EXPECT_FALSE(NetAddress{}.is_routable());
}

TEST(Sockets, LoopbackConnection) {
    const Socket listener = Socket::listen(0, true);
    const std::uint16_t port = listener.local_port();
    ASSERT_NE(port, 0);
    const Socket client = Socket::connect(NetAddress::ipv4(127, 0, 0, 1, port));

    std::optional<std::pair<Socket, NetAddress>> server;
    for (int i = 0; i < 100 && !server; ++i) {
        PollItem items[] = {{listener.handle(), true, false}};
        poll(items, 50);
        server = listener.accept();
    }
    ASSERT_TRUE(server.has_value());
    PollItem connected[] = {{client.handle(), false, true}};
    poll(connected, 1000);
    ASSERT_TRUE(connected[0].writable);
    EXPECT_EQ(client.connect_error(), 0);

    const Bytes hello{'h', 'e', 'l', 'l', 'o'};
    EXPECT_EQ(client.send(hello).bytes, 5u);
    PollItem readable[] = {{server->first.handle(), true, false}};
    poll(readable, 1000);
    std::array<std::uint8_t, 16> buffer{};
    const IoResult r = server->first.recv(buffer);
    ASSERT_EQ(r.bytes, 5u);
    EXPECT_EQ(Bytes(buffer.begin(), buffer.begin() + 5), hello);
}

TEST(Sockets, APortListenedOnIsTaken) {
    const Socket first = Socket::listen(0, true);
    EXPECT_THROW(Socket::listen(first.local_port(), true), std::runtime_error);
}

// Two whole nodes, each with its own network thread, over real TCP on 127.0.0.1.
TEST(Connman, TwoNodesSyncOverTcp) {
    const ConsensusParams params = easy_params();
    Chainstate chain_a(params);
    Chainstate chain_b(params);
    Mempool mempool_a(chain_a);
    Mempool mempool_b(chain_b);
    mine_blocks(chain_a, test_key("alice").public_key(), 20);
    AddrMan addrs_a(1);
    AddrMan addrs_b(2);
    BanMan bans_a;
    BanMan bans_b;
    PeerManager peers_a(chain_a, mempool_a, addrs_a, bans_a, {}, 1);
    PeerManager peers_b(chain_b, mempool_b, addrs_b, bans_b, {}, 2);
    const Connman::Clock clock = [] { return kTestNow; };  // the test chain's blocks are dated in 2027

    ConnmanOptions listening;
    listening.loopback_only = true;
    Connman net_a(peers_a, addrs_a, bans_a, listening, {}, clock);
    ConnmanOptions dialling;
    dialling.listen = false;
    dialling.connect = {std::format("127.0.0.1:{}", net_a.port())};
    Connman net_b(peers_b, addrs_b, bans_b, dialling, {}, clock);

    std::atomic<int> height_b{0};
    std::thread thread_a([&] { net_a.run(); });
    std::thread thread_b([&] { net_b.run([&] { height_b = chain_b.height(); }); });
    for (int i = 0; i < 400 && height_b < 20; ++i) std::this_thread::sleep_for(50ms);
    net_a.stop();
    net_b.stop();
    thread_a.join();
    thread_b.join();
    EXPECT_EQ(chain_b.height(), 20);
    EXPECT_EQ(chain_b.tip().hash, chain_a.tip().hash);
}

// Milestone 8: a node that knows nobody asks its seeds. Here the seed is a name, "localhost",
// that leads to the other node; no --connect, no known addresses.
TEST(Connman, FindsPeersThroughSeeds) {
    const ConsensusParams params = easy_params();
    Chainstate chain_a(params);
    Chainstate chain_b(params);
    Mempool mempool_a(chain_a);
    Mempool mempool_b(chain_b);
    mine_blocks(chain_a, test_key("alice").public_key(), 8);
    AddrMan addrs_a(1);
    AddrMan addrs_b(2);
    BanMan bans_a;
    BanMan bans_b;
    PeerManager peers_a(chain_a, mempool_a, addrs_a, bans_a, {}, 1);
    PeerManager peers_b(chain_b, mempool_b, addrs_b, bans_b, {}, 2);
    const Connman::Clock clock = [] { return kTestNow; };

    ConnmanOptions listening;
    listening.loopback_only = true;
    Connman net_a(peers_a, addrs_a, bans_a, listening, {}, clock);
    ConnmanOptions seeded;
    seeded.listen = false;
    seeded.seeds = {std::format("localhost:{}", net_a.port())};
    Connman net_b(peers_b, addrs_b, bans_b, seeded, {}, clock);

    std::atomic<int> height_b{0};
    std::thread thread_a([&] { net_a.run(); });
    std::thread thread_b([&] { net_b.run([&] { height_b = chain_b.height(); }); });
    for (int i = 0; i < 400 && height_b < 8; ++i) std::this_thread::sleep_for(50ms);
    net_a.stop();
    net_b.stop();
    thread_a.join();
    thread_b.join();
    EXPECT_EQ(chain_b.height(), 8);
    EXPECT_NE(addrs_b.find(NetAddress::ipv4(127, 0, 0, 1, net_a.port())), nullptr);  // learned from the seed
}

// A banned address is let in by the OS and closed at once, so it never gets to sync.
TEST(Connman, BannedAddressesCantConnect) {
    const ConsensusParams params = easy_params();
    Chainstate chain_a(params);
    Chainstate chain_b(params);
    Mempool mempool_a(chain_a);
    Mempool mempool_b(chain_b);
    mine_blocks(chain_a, test_key("alice").public_key(), 5);
    AddrMan addrs_a(1);
    AddrMan addrs_b(2);
    BanMan bans_a;
    BanMan bans_b;
    bans_a.ban(NetAddress::ipv4(127, 0, 0, 1, 0), kTestNow + 3600);
    PeerManager peers_a(chain_a, mempool_a, addrs_a, bans_a, {}, 1);
    PeerManager peers_b(chain_b, mempool_b, addrs_b, bans_b, {}, 2);
    const Connman::Clock clock = [] { return kTestNow; };

    ConnmanOptions listening;
    listening.loopback_only = true;
    Connman net_a(peers_a, addrs_a, bans_a, listening, {}, clock);
    ConnmanOptions dialling;
    dialling.listen = false;
    dialling.connect = {std::format("127.0.0.1:{}", net_a.port())};
    Connman net_b(peers_b, addrs_b, bans_b, dialling, {}, clock);

    std::thread thread_a([&] { net_a.run(); });
    std::thread thread_b([&] { net_b.run(); });
    std::this_thread::sleep_for(1500ms);
    net_a.stop();
    net_b.stop();
    thread_a.join();
    thread_b.join();
    EXPECT_EQ(chain_b.height(), 0);
}
