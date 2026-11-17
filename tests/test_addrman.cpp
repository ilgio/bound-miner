#include "net/addrman.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <optional>
#include <set>

using namespace bound;

namespace {

NetAddress addr(std::uint8_t last, std::uint16_t port = 18333) { return NetAddress::ipv4(10, 0, 0, last, port); }

constexpr std::int64_t kNow = kTestNow;

}  // namespace

TEST(AddrMan, AddsAndRefreshes) {
    AddrMan man(1);
    EXPECT_TRUE(man.add(addr(1), kNow - 100));
    EXPECT_FALSE(man.add(addr(1), kNow));  // known: refreshed, not added
    ASSERT_NE(man.find(addr(1)), nullptr);
    EXPECT_EQ(man.find(addr(1))->last_seen, kNow);
    EXPECT_FALSE(man.add(addr(1), kNow - 500));  // an older sighting doesn't move it back
    EXPECT_EQ(man.find(addr(1))->last_seen, kNow);
    EXPECT_TRUE(man.add(addr(1, 1234), kNow));  // another port is another node
    EXPECT_EQ(man.size(), 2u);
}

TEST(AddrMan, RefusesUnusableAddresses) {
    AddrMan man(1);
    EXPECT_FALSE(man.add(NetAddress::ipv4(0, 0, 0, 0, 18333), kNow));
    EXPECT_FALSE(man.add(NetAddress{}, kNow));
    EXPECT_FALSE(man.add(addr(1, 0), kNow));
    EXPECT_EQ(man.size(), 0u);
}

TEST(AddrMan, SelectSkipsExcludedAndBacksOff) {
    AddrMan man(1);
    man.add(addr(1), kNow);
    man.add(addr(2), kNow);
    EXPECT_EQ(man.select({addr(1)}, kNow), addr(2));
    EXPECT_EQ(man.select({addr(1), addr(2)}, kNow), std::nullopt);

    man.attempt(addr(2), kNow);  // a failed attempt: wait 2 minutes before the next
    EXPECT_EQ(man.select({addr(1)}, kNow + 119), std::nullopt);
    EXPECT_EQ(man.select({addr(1)}, kNow + 120), addr(2));
    man.attempt(addr(2), kNow + 120);  // two failures: 4 minutes
    EXPECT_EQ(man.select({addr(1)}, kNow + 120 + 239), std::nullopt);
    EXPECT_EQ(man.select({addr(1)}, kNow + 120 + 240), addr(2));

    man.good(addr(2), kNow + 400);  // a success clears the record
    EXPECT_EQ(man.find(addr(2))->attempts, 0);
    EXPECT_EQ(man.find(addr(2))->last_success, kNow + 400);
}

TEST(AddrMan, SampleIsRandomAndBounded) {
    AddrMan man(1);
    for (std::uint8_t i = 1; i <= 50; ++i) man.add(addr(i), kNow);
    const std::vector<AddrInfo> some = man.sample(10);
    EXPECT_EQ(some.size(), 10u);
    std::set<NetAddress> distinct;
    for (const AddrInfo& info : some) distinct.insert(info.addr);
    EXPECT_EQ(distinct.size(), 10u);
    EXPECT_EQ(man.sample(1000).size(), 50u);
}

TEST(AddrMan, EvictsTheOldestWhenFull) {
    AddrMan man(1);
    for (std::size_t i = 0; i < AddrMan::kMaxSize; ++i) {
        man.add(NetAddress::ipv4(10, static_cast<std::uint8_t>(i >> 8), static_cast<std::uint8_t>(i), 1, 18333),
                kNow + static_cast<std::int64_t>(i));
    }
    ASSERT_EQ(man.size(), AddrMan::kMaxSize);
    const NetAddress oldest = NetAddress::ipv4(10, 0, 0, 1, 18333);
    ASSERT_NE(man.find(oldest), nullptr);
    EXPECT_TRUE(man.add(NetAddress::ipv4(192, 168, 0, 1, 18333), kNow + 1'000'000));
    EXPECT_EQ(man.size(), AddrMan::kMaxSize);
    EXPECT_EQ(man.find(oldest), nullptr);
}

// Milestone 8: an address book that strangers write into.

TEST(AddrMan, KeepsProvenAddressesWhenFull) {
    AddrMan man(1);
    const NetAddress proven = NetAddress::ipv4(10, 200, 0, 1, 18333);
    man.add(proven, kNow - 2'000'000);
    man.good(proven, kNow - 1'000'000);  // it answered once, long before everything below was seen
    for (std::size_t i = 0; i + 1 < AddrMan::kMaxSize; ++i) {
        man.add(NetAddress::ipv4(10, static_cast<std::uint8_t>(i >> 8), static_cast<std::uint8_t>(i), 1, 18333),
                kNow + static_cast<std::int64_t>(i));
    }
    ASSERT_EQ(man.size(), AddrMan::kMaxSize);
    // A flood of fresh addresses pushes out unproven ones, oldest first; never the proven one.
    for (std::uint8_t j = 0; j < 100; ++j) EXPECT_TRUE(man.add(NetAddress::ipv4(192, 168, j, 1, 18333), kNow + 2'000'000 + j));
    EXPECT_EQ(man.size(), AddrMan::kMaxSize);
    EXPECT_NE(man.find(proven), nullptr);
    EXPECT_EQ(man.find(NetAddress::ipv4(10, 0, 0, 1, 18333)), nullptr);
}

TEST(AddrMan, ForgetsHopelessAddresses) {
    constexpr std::int64_t kDay = 24 * 60 * 60;
    AddrMan man(1);
    man.add(addr(1), kNow);
    man.add(addr(2), kNow);
    man.add(addr(3), kNow - 31 * kDay);
    man.add(addr(4), kNow);
    man.good(addr(4), kNow);
    for (int i = 0; i < 3; ++i) man.attempt(addr(1), kNow + i);
    for (int i = 0; i < 10; ++i) man.attempt(addr(4), kNow + i);

    EXPECT_TRUE(AddrMan::is_terrible(*man.find(addr(1)), kNow + 10));   // three tries, never an answer
    EXPECT_FALSE(AddrMan::is_terrible(*man.find(addr(2)), kNow + 10));
    EXPECT_TRUE(AddrMan::is_terrible(*man.find(addr(3)), kNow));        // not seen for a month
    EXPECT_FALSE(AddrMan::is_terrible(*man.find(addr(4)), kNow + 6 * kDay));  // it did answer this week
    EXPECT_TRUE(AddrMan::is_terrible(*man.find(addr(4)), kNow + 8 * kDay));

    EXPECT_EQ(man.sample(100, kNow + 10).size(), 2u);  // not handed out
    man.select({}, kNow + 10);                          // and dropped when next looking for one to dial
    EXPECT_EQ(man.find(addr(1)), nullptr);
    EXPECT_EQ(man.find(addr(3)), nullptr);
    EXPECT_NE(man.find(addr(2)), nullptr);
    EXPECT_NE(man.find(addr(4)), nullptr);
}

// Made-up addresses can outnumber real ones a hundred to one; half the dials still go to proven ones.
TEST(AddrMan, DialsProvenAddressesHalfTheTime) {
    AddrMan man(7);
    const NetAddress proven = addr(200);
    man.add(proven, kNow);
    man.good(proven, kNow);
    for (std::uint8_t i = 1; i <= 100; ++i) man.add(addr(i), kNow);
    int hits = 0;
    for (int i = 0; i < 400; ++i) hits += man.select({}, kNow) == proven ? 1 : 0;
    EXPECT_GT(hits, 150);
    EXPECT_LT(hits, 250);
}

TEST(AddrMan, RoutableOnlyKeepsPrivateAddressesOut) {
    AddrMan man(1);
    man.add(addr(1), kNow);  // 10.0.0.1
    man.add(NetAddress::ipv4(8, 8, 8, 8, 18333), kNow);
    man.set_routable_only(true);
    EXPECT_EQ(man.find(addr(1)), nullptr);  // dropped when the switch went on
    EXPECT_NE(man.find(NetAddress::ipv4(8, 8, 8, 8, 18333)), nullptr);
    EXPECT_FALSE(man.add(NetAddress::ipv4(192, 168, 1, 1, 18333), kNow));
    EXPECT_FALSE(man.add(NetAddress::ipv4(127, 0, 0, 1, 18333), kNow));
    EXPECT_TRUE(man.add(NetAddress::ipv4(1, 1, 1, 1, 18333), kNow));
}

TEST(AddrMan, RoundTripsThroughPeersDat) {
    AddrMan man(1);
    man.add(addr(1), kNow);
    man.add(NetAddress{{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 18333}, kNow - 5);  // IPv6
    man.attempt(addr(1), kNow + 1);
    man.good(addr(1), kNow + 2);
    man.attempt(addr(1), kNow + 3);

    const AddrMan back = AddrMan::deserialize(man.serialize(), 2);
    ASSERT_EQ(back.size(), 2u);
    EXPECT_EQ(*back.find(addr(1)), *man.find(addr(1)));
    Bytes truncated = man.serialize();
    truncated.pop_back();
    EXPECT_THROW(AddrMan::deserialize(truncated, 2), DeserializeError);
}
