#include "core/validation.h"
#include "test_util.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

using namespace bound;

namespace {

// A hand-built UTXO set holding the two coins rent_payment() spends: 3 + 2 BOUND, both Alice's,
// created at height 10. rent_payment() pays out 4.9999 BOUND, leaving a 0.0001 BOUND fee.
class ValidationTest : public ::testing::Test {
protected:
    static constexpr Amount kFee = 10'000;

    void SetUp() override {
        utxos.add(out_a, Coin{TxOut{3 * kCoin, alice}, 10, false});
        utxos.add(out_b, Coin{TxOut{2 * kCoin, alice}, 10, false});
    }

    static Transaction payment() {
        Transaction tx = rent_payment();
        sign_all_inputs(tx, test_key("alice"));
        return tx;
    }

    // A mined block whose coinbase (for `coinbase_height`) claims `claim`, followed by `txs`.
    Block block_with(int coinbase_height, Amount claim, std::vector<Transaction> txs) const {
        Block block;
        block.header.prev_hash = test_hash("parent");
        block.header.time = kTestGenesisTime;
        block.header.bits = params.pow_limit_bits;
        block.txs.push_back(make_coinbase(coinbase_height, claim, miner, {}));
        for (Transaction& tx : txs) block.txs.push_back(std::move(tx));
        block.header.merkle_root = block.compute_merkle_root();
        block.header = mine(block.header, params).value();
        return block;
    }

    const ConsensusParams params = easy_params();
    const PublicKey alice = test_key("alice").public_key();
    const PublicKey bob = test_key("bob").public_key();
    const PublicKey miner = test_key("miner").public_key();
    const OutPoint out_a{test_hash("funding tx a"), 0};
    const OutPoint out_b{test_hash("funding tx b"), 3};
    UtxoSet utxos;
};

}  // namespace

TEST_F(ValidationTest, InputsOkComputesTheFee) {
    Amount fee = -1;
    EXPECT_EQ(check_tx_inputs(payment(), utxos, 11, params, &fee), InputsCheck::Ok);
    EXPECT_EQ(fee, kFee);
}

TEST_F(ValidationTest, InputsMustExist) {
    utxos.spend(out_b);
    EXPECT_EQ(check_tx_inputs(payment(), utxos, 11, params, nullptr), InputsCheck::MissingInput);
}

TEST_F(ValidationTest, CoinbaseOutputsMustMature) {
    utxos.spend(out_a);
    utxos.add(out_a, Coin{TxOut{3 * kCoin, alice}, 10, true});  // now a coinbase output from height 10
    EXPECT_EQ(check_tx_inputs(payment(), utxos, 109, params, nullptr), InputsCheck::ImmatureCoinbase);  // 99 deep
    EXPECT_EQ(check_tx_inputs(payment(), utxos, 110, params, nullptr), InputsCheck::Ok);                // 100 deep
}

TEST_F(ValidationTest, InputValuesMustStayInRange) {
    utxos.spend(out_a);
    utxos.add(out_a, Coin{TxOut{kMaxMoney, alice}, 10, false});
    EXPECT_EQ(check_tx_inputs(payment(), utxos, 11, params, nullptr), InputsCheck::InputValueOutOfRange);
}

TEST_F(ValidationTest, CantSpendMoreThanTheInputs) {
    utxos.spend(out_a);
    utxos.add(out_a, Coin{TxOut{1 * kCoin, alice}, 10, false});  // 3 BOUND in, 4.9999 out
    EXPECT_EQ(check_tx_inputs(payment(), utxos, 11, params, nullptr), InputsCheck::InputsBelowOutputs);
}

TEST_F(ValidationTest, SignaturesMustBeTheOwners) {
    Transaction by_bob = rent_payment();
    sign_all_inputs(by_bob, test_key("bob"));
    EXPECT_EQ(check_tx_inputs(by_bob, utxos, 11, params, nullptr), InputsCheck::BadSignature);

    utxos.spend(out_a);
    utxos.add(out_a, Coin{TxOut{3 * kCoin, bob}, 10, false});  // coin a now belongs to Bob
    EXPECT_EQ(check_tx_inputs(payment(), utxos, 11, params, nullptr), InputsCheck::BadSignature);
}

TEST_F(ValidationTest, ConnectAppliesTheBlock) {
    const Block block = block_with(11, 5 * kCoin + kFee, {payment()});
    BlockUndo undo;
    ASSERT_EQ(connect_block(block, 11, utxos, undo, params), ConnectResult::Ok);

    EXPECT_FALSE(utxos.contains(out_a));
    EXPECT_FALSE(utxos.contains(out_b));
    const uint256 txid = block.txs[1].txid();
    EXPECT_EQ(*utxos.find({txid, 0}), (Coin{TxOut{150'000'000, bob}, 11, false}));
    EXPECT_EQ(*utxos.find({txid, 1}), (Coin{TxOut{349'990'000, alice}, 11, false}));
    EXPECT_EQ(*utxos.find({block.txs[0].txid(), 0}), (Coin{TxOut{5 * kCoin + kFee, miner}, 11, true}));
    EXPECT_EQ(utxos.total_value(), 10 * kCoin);  // the 5 that were there, plus a 5 BOUND subsidy

    const std::vector<Coin> spent = {Coin{TxOut{3 * kCoin, alice}, 10, false}, Coin{TxOut{2 * kCoin, alice}, 10, false}};
    EXPECT_EQ(undo.spent, spent);
}

TEST_F(ValidationTest, DisconnectRestoresTheExactUtxoSet) {
    const UtxoSet before = utxos;
    const Block block = block_with(11, 5 * kCoin + kFee, {payment()});
    BlockUndo undo;
    ASSERT_EQ(connect_block(block, 11, utxos, undo, params), ConnectResult::Ok);
    disconnect_block(block, undo, utxos);
    EXPECT_TRUE(utxos == before);
}

TEST_F(ValidationTest, CoinbaseMayClaimSubsidyPlusFees) {
    BlockUndo undo;
    const UtxoSet before = utxos;
    EXPECT_EQ(connect_block(block_with(11, 5 * kCoin + kFee + 1, {payment()}), 11, utxos, undo, params),
              ConnectResult::CoinbaseTooLarge);
    EXPECT_TRUE(utxos == before);  // rolled back
    EXPECT_EQ(connect_block(block_with(11, 5 * kCoin + kFee, {payment()}), 11, utxos, undo, params), ConnectResult::Ok);
}

TEST_F(ValidationTest, CoinbaseMayClaimLess) {  // the difference is simply never created
    BlockUndo undo;
    EXPECT_EQ(connect_block(block_with(11, 1, {payment()}), 11, utxos, undo, params), ConnectResult::Ok);
}

TEST_F(ValidationTest, CoinbaseMustCarryItsHeight) {
    BlockUndo undo;
    EXPECT_EQ(connect_block(block_with(12, 5 * kCoin, {}), 11, utxos, undo, params), ConnectResult::BadCoinbaseHeight);
}

TEST_F(ValidationTest, DoubleSpendInsideABlockIsRejectedAndRolledBack) {
    Transaction again;  // spends coin a a second time
    again.inputs = {out_a};
    again.outputs = {{kCoin, bob}};
    sign_all_inputs(again, test_key("alice"));

    const UtxoSet before = utxos;
    BlockUndo undo;
    EXPECT_EQ(connect_block(block_with(11, 5 * kCoin, {payment(), again}), 11, utxos, undo, params),
              ConnectResult::BadInputs);
    EXPECT_TRUE(utxos == before);
}

TEST_F(ValidationTest, CanSpendAnOutputFromEarlierInTheSameBlock) {
    const Transaction first = payment();
    Transaction second;  // spends Alice's change from `first`
    second.inputs = {{first.txid(), 1}};
    second.outputs = {{349'980'000, bob}};
    sign_all_inputs(second, test_key("alice"));

    const UtxoSet before = utxos;
    const Block block = block_with(11, 5 * kCoin + 2 * kFee, {first, second});
    BlockUndo undo;
    ASSERT_EQ(connect_block(block, 11, utxos, undo, params), ConnectResult::Ok);
    EXPECT_FALSE(utxos.contains({first.txid(), 1}));
    EXPECT_TRUE(utxos.contains({second.txid(), 0}));

    disconnect_block(block, undo, utxos);  // must undo `second` before `first`
    EXPECT_TRUE(utxos == before);
}

TEST_F(ValidationTest, RunsTheContextFreeChecksToo) {
    Block block = block_with(11, 5 * kCoin + kFee, {payment()});
    block.header.merkle_root = test_hash("wrong");
    block.header = mine(block.header, params).value();
    BlockUndo undo;
    EXPECT_EQ(connect_block(block, 11, utxos, undo, params), ConnectResult::BadBlock);
}

TEST_F(ValidationTest, NeverOverwritesAnUnspentOutput) {
    const Block block = block_with(11, 5 * kCoin + kFee, {payment()});
    utxos.add({block.txs[0].txid(), 0}, Coin{TxOut{1, alice}, 3, true});  // only a hash collision could do this
    const UtxoSet before = utxos;
    BlockUndo undo;
    EXPECT_EQ(connect_block(block, 11, utxos, undo, params), ConnectResult::DuplicateOutput);
    EXPECT_TRUE(utxos == before);
}
