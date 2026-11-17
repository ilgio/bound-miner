#include "core/coins.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <functional>

using namespace bound;

namespace {

Coin coin(Amount amount, int height = 1, bool coinbase = false) {
    return Coin{TxOut{amount, test_key("alice").public_key()}, height, coinbase};
}

}  // namespace

TEST(UtxoSet, AddFindSpend) {
    UtxoSet utxos;
    const OutPoint op{test_hash("tx"), 0};
    EXPECT_FALSE(utxos.contains(op));
    EXPECT_TRUE(utxos.add(op, coin(7)));
    ASSERT_NE(utxos.find(op), nullptr);
    EXPECT_EQ(*utxos.find(op), coin(7));
    EXPECT_EQ(utxos.size(), 1u);

    EXPECT_EQ(utxos.spend(op), coin(7));
    EXPECT_FALSE(utxos.contains(op));
    EXPECT_EQ(utxos.size(), 0u);
}

TEST(UtxoSet, NeverOverwritesAnUnspentCoin) {
    UtxoSet utxos;
    const OutPoint op{test_hash("tx"), 0};
    EXPECT_TRUE(utxos.add(op, coin(7)));
    EXPECT_FALSE(utxos.add(op, coin(8)));
    EXPECT_EQ(utxos.find(op)->out.amount, 7);
}

TEST(UtxoSet, SpendingWhatIsntThere) {
    UtxoSet utxos;
    EXPECT_FALSE(utxos.spend(OutPoint{test_hash("tx"), 0}).has_value());
}

TEST(UtxoSet, OutputIndexMatters) {
    UtxoSet utxos;
    utxos.add(OutPoint{test_hash("tx"), 0}, coin(1));
    EXPECT_TRUE(utxos.add(OutPoint{test_hash("tx"), 1}, coin(2)));
    EXPECT_FALSE(utxos.contains(OutPoint{test_hash("tx"), 2}));
    const std::hash<OutPoint> h;
    EXPECT_NE(h(OutPoint{test_hash("tx"), 0}), h(OutPoint{test_hash("tx"), 1}));
}

TEST(UtxoSet, TotalValueAndEquality) {
    UtxoSet a;
    a.add(OutPoint{test_hash("x"), 0}, coin(5 * kCoin));
    a.add(OutPoint{test_hash("y"), 0}, coin(2 * kCoin));
    EXPECT_EQ(a.total_value(), 7 * kCoin);

    UtxoSet b;  // same coins, added in the other order
    b.add(OutPoint{test_hash("y"), 0}, coin(2 * kCoin));
    b.add(OutPoint{test_hash("x"), 0}, coin(5 * kCoin));
    EXPECT_TRUE(a == b);
    b.spend(OutPoint{test_hash("x"), 0});
    EXPECT_FALSE(a == b);
}
