#include "net/banman.h"
#include "net/socket.h"
#include "test_util.h"
#include "util/serialize.h"

#include <gtest/gtest.h>

using namespace bound;

TEST(BanMan, DiscouragementLastsADay) {
    BanMan bans;
    const NetAddress peer = NetAddress::ipv4(10, 1, 2, 3, 18333);
    EXPECT_FALSE(bans.refuses(peer, kTestNow));
    bans.discourage(peer, kTestNow);
    EXPECT_TRUE(bans.is_discouraged(peer, kTestNow));
    EXPECT_TRUE(bans.is_discouraged(NetAddress::ipv4(10, 1, 2, 3, 40000), kTestNow));  // any port
    EXPECT_FALSE(bans.is_discouraged(NetAddress::ipv4(10, 1, 2, 4, 18333), kTestNow));
    EXPECT_TRUE(bans.refuses(peer, kTestNow + BanMan::kDiscourageTime - 1));
    EXPECT_FALSE(bans.refuses(peer, kTestNow + BanMan::kDiscourageTime));
    EXPECT_FALSE(bans.is_banned(peer, kTestNow));
}

TEST(BanMan, BansComeAndGo) {
    BanMan bans;
    const NetAddress a = NetAddress::ipv4(10, 0, 0, 1, 1);
    const NetAddress b = resolve("[2001:db8::7]:5", 1).at(0);
    bans.ban(a, kTestNow + 100);
    bans.ban(b, kTestNow + 200);
    EXPECT_TRUE(bans.is_banned(a, kTestNow + 99));
    EXPECT_FALSE(bans.is_banned(a, kTestNow + 100));
    EXPECT_TRUE(bans.refuses(b, kTestNow));
    ASSERT_EQ(bans.banned(kTestNow).size(), 2u);
    EXPECT_EQ(bans.banned(kTestNow + 150).size(), 1u);  // a's ban is over, and dropped
    EXPECT_EQ(bans.banned(kTestNow + 150)[0].first.ip_string(), "2001:db8::7");
    EXPECT_TRUE(bans.unban(b));
    EXPECT_FALSE(bans.unban(b));
    bans.ban(a, kTestNow + 100);
    bans.clear_banned();
    EXPECT_TRUE(bans.banned(kTestNow).empty());
}

TEST(BanMan, OnlyBansAreSaved) {
    BanMan bans;
    const NetAddress banned = NetAddress::ipv4(10, 0, 0, 1, 1);
    const NetAddress discouraged = NetAddress::ipv4(10, 0, 0, 2, 1);
    bans.ban(banned, kTestNow + 1000);
    bans.discourage(discouraged, kTestNow);
    const Bytes saved = bans.serialize();
    EXPECT_EQ(saved.size(), 1u + 1 + 16 + 8);
    BanMan loaded = BanMan::deserialize(saved);
    EXPECT_TRUE(loaded.is_banned(banned, kTestNow));
    EXPECT_FALSE(loaded.is_discouraged(discouraged, kTestNow));
    EXPECT_EQ(loaded.serialize(), saved);

    Bytes wrong_version = saved;
    wrong_version[0] = 9;
    EXPECT_THROW(BanMan::deserialize(wrong_version), DeserializeError);
    EXPECT_THROW(BanMan::deserialize(Bytes{1, 5}), DeserializeError);  // 5 entries promised, none there
    Bytes trailing = saved;
    trailing.push_back(0);
    EXPECT_THROW(BanMan::deserialize(trailing), DeserializeError);
}

TEST(Sockets, LoopbackAddresses) {
    EXPECT_TRUE(NetAddress::ipv4(127, 0, 0, 1, 1).is_loopback());
    EXPECT_TRUE(NetAddress::ipv4(127, 8, 9, 10, 1).is_loopback());
    EXPECT_TRUE(resolve("[::1]:1", 1).at(0).is_loopback());
    EXPECT_FALSE(NetAddress::ipv4(10, 0, 0, 1, 1).is_loopback());
    EXPECT_FALSE(resolve("[::2]:1", 1).at(0).is_loopback());
    EXPECT_EQ(NetAddress::ipv4(1, 2, 3, 4, 5).ip_string(), "1.2.3.4");
}
