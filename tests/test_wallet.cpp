#include "core/consensus.h"
#include "core/validation.h"
#include "core/wallet.h"
#include "test_util.h"

#include <gtest/gtest.h>

using namespace bound;

namespace {

class WalletTest : public ::testing::Test {
protected:
    void SetUp() override {
        utxos.add(older, Coin{TxOut{3 * kCoin, alice.public_key()}, 10, false});
        utxos.add(newer, Coin{TxOut{2 * kCoin, alice.public_key()}, 11, false});
        utxos.add(bobs, Coin{TxOut{9 * kCoin, bob.public_key()}, 10, false});
        utxos.add(fresh_coinbase, Coin{TxOut{5 * kCoin, alice.public_key()}, 150, true});
    }

    const ConsensusParams params = easy_params();
    const KeyPair alice = test_key("alice");
    const KeyPair bob = test_key("bob");
    const OutPoint older{test_hash("older"), 0};
    const OutPoint newer{test_hash("newer"), 0};
    const OutPoint bobs{test_hash("bobs"), 0};
    const OutPoint fresh_coinbase{test_hash("coinbase"), 0};
    UtxoSet utxos;
};

}  // namespace

TEST_F(WalletTest, BalanceCountsOnlyTheOwnersCoins) {
    EXPECT_EQ(balance(utxos, alice.public_key()), 10 * kCoin);  // immature coins still count
    EXPECT_EQ(balance(utxos, bob.public_key()), 9 * kCoin);
    EXPECT_EQ(balance(utxos, test_key("carol").public_key()), 0);
}

TEST_F(WalletTest, SpendableCoinsAreMatureAndOldestFirst) {
    const auto at_200 = spendable_coins(utxos, alice.public_key(), 200, params);
    ASSERT_EQ(at_200.size(), 2u);  // the coinbase from height 150 is only 50 deep
    EXPECT_EQ(at_200[0].first, older);
    EXPECT_EQ(at_200[1].first, newer);
    EXPECT_EQ(spendable_coins(utxos, alice.public_key(), 250, params).size(), 3u);
}

TEST_F(WalletTest, PaymentWithChange) {
    const std::optional<Transaction> tx = create_payment(utxos, alice, bob.public_key(), 4 * kCoin, kCoin / 10, 200, params);
    ASSERT_TRUE(tx.has_value());
    EXPECT_EQ(tx->inputs, (std::vector<OutPoint>{older, newer}));
    ASSERT_EQ(tx->outputs.size(), 2u);
    EXPECT_EQ(tx->outputs[0], (TxOut{4 * kCoin, bob.public_key()}));
    EXPECT_EQ(tx->outputs[1], (TxOut{9 * kCoin / 10, alice.public_key()}));  // change

    Amount fee = 0;
    EXPECT_EQ(check_transaction(*tx), TxCheck::Ok);
    EXPECT_EQ(check_tx_inputs(*tx, utxos, 200, params, &fee), InputsCheck::Ok);
    EXPECT_EQ(fee, kCoin / 10);
}

TEST_F(WalletTest, ExactAmountNeedsNoChange) {
    const std::optional<Transaction> tx = create_payment(utxos, alice, bob.public_key(), 2 * kCoin + kCoin * 9 / 10, kCoin / 10, 200, params);
    ASSERT_TRUE(tx.has_value());
    EXPECT_EQ(tx->inputs.size(), 1u);  // the 3 BOUND coin covers it exactly
    EXPECT_EQ(tx->outputs.size(), 1u);
}

TEST_F(WalletTest, RefusesWhatItCantPay) {
    EXPECT_FALSE(create_payment(utxos, alice, bob.public_key(), 5 * kCoin, 1, 200, params));  // only 5 mature
    EXPECT_FALSE(create_payment(utxos, alice, bob.public_key(), 0, 0, 200, params));
    EXPECT_FALSE(create_payment(utxos, alice, bob.public_key(), kCoin, -1, 200, params));
    EXPECT_TRUE(create_payment(utxos, alice, bob.public_key(), 5 * kCoin, 1, 250, params));  // coinbase now mature
}
