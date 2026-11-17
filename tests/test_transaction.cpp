#include "core/transaction.h"
#include "test_util.h"
#include "util/hex.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

using namespace bound;

namespace {

// Expected values for rent_payment() (test_util.h), computed independently in Python.
constexpr const char* kBodyHex =
    "01000000"                                                          // version
    "02"                                                                // 2 inputs
    "3362162dc7cca409173a8eef12f7d13da415bd52bdb36a2a0bfe274cebc2ccbf" "00000000"  // funding tx a : 0
    "5eb8bc511fb060ee5204ce1a7ddca8b9aecb8d43a04703bc2fa21c5a54300493" "03000000"  // funding tx b : 3
    "02"                                                                // 2 outputs
    "80d1f00800000000"                                                  // 1.5 BOUND
    "ecc1b58727f3f12b3194881a9ecb9de0b28ce7b207230d8e930fe1bce75e256c"  //   to bob
    "706cdc1400000000"                                                  // 3.4999 BOUND
    "d5bf4a3fcce717b0388bcc2749ebc148ad9969b23f45ee1b605fd58778576ac4"  //   to alice
    "00000000"                                                          // lock_time
    "04" "72656e74";                                                    // memo "rent"
constexpr const char* kTxid = "c9a65c6fd805e2a7df5798abce3dd153a82f6c2b557301bbc193e0b9e4a0fb84";
constexpr const char* kSighash = "22c085acdfe2065ea0e1acc3895280ed8e5e43fb13ab1dff004c9b1a68ab5e02";
constexpr const char* kAliceSig =
    "e7e2d8bf8ef8d46c0fea4e8226ad56d970548b6b09ae403d755d98460074b79d"
    "022b23f0c03a2ec2f4c6964603b4c8672f59ec2ccf1c38ec82a61d84a2d2db09";
constexpr const char* kWtxid = "eee52d8482838d9edd3a7f7bcf6d9bd19c2bf777dbe84bfb36d16fa830f57613";

Transaction signed_by_alice() {
    Transaction tx = rent_payment();
    sign_all_inputs(tx, test_key("alice"));
    return tx;
}

}  // namespace

TEST(Transaction, BodySerialization) {
    EXPECT_EQ(to_hex(rent_payment().serialize_body()), kBodyHex);
}

TEST(Transaction, Txid) {
    EXPECT_EQ(rent_payment().txid().to_hex(), kTxid);
}

TEST(Transaction, Sighash) {
    EXPECT_EQ(rent_payment().sighash().to_hex(), kSighash);
}

TEST(Transaction, SignAllInputs) {
    const Transaction tx = signed_by_alice();
    ASSERT_EQ(tx.signatures.size(), 2u);
    EXPECT_EQ(to_hex(tx.signatures[0]), kAliceSig);  // Ed25519 is deterministic
    EXPECT_EQ(to_hex(tx.signatures[1]), kAliceSig);
    EXPECT_EQ(tx.wtxid().to_hex(), kWtxid);
}

TEST(Transaction, FullSerializationAppendsSignatures) {
    const Bytes full = signed_by_alice().serialize();
    EXPECT_EQ(full.size(), 295u);
    EXPECT_EQ(to_hex(full), std::string(kBodyHex) + kAliceSig + kAliceSig);
}

// txid is what outpoints refer to, so nobody may change it without the owner's key. wtxid is what
// the block commits to, so it must cover the signatures too.
TEST(Transaction, TxidIgnoresSignaturesWtxidDoesNot) {
    Transaction tx = rent_payment();
    const uint256 txid = tx.txid();
    sign_all_inputs(tx, test_key("alice"));
    EXPECT_EQ(tx.txid(), txid);
    const uint256 wtxid = tx.wtxid();
    sign_all_inputs(tx, test_key("bob"));
    EXPECT_EQ(tx.txid(), txid);
    EXPECT_NE(tx.wtxid(), wtxid);
}

TEST(Transaction, RoundTrips) {
    const Transaction tx = signed_by_alice();
    EXPECT_EQ(Transaction::deserialize(tx.serialize()), tx);

    Transaction coinbase;
    coinbase.outputs = {{5 * kCoin, test_key("alice").public_key()}};
    coinbase.lock_time = 7;
    EXPECT_EQ(Transaction::deserialize(coinbase.serialize()), coinbase);
}

TEST(Transaction, SerializeNeedsOneSignaturePerInput) {
    Transaction tx = signed_by_alice();
    EXPECT_EQ(tx.serialize().size(), 295u);  // fine with one signature per input...
    tx.signatures.pop_back();
    EXPECT_THROW(tx.serialize(), std::logic_error);  // ...a precondition violation without
}

TEST(Transaction, DeserializeRejectsTruncatedOrTrailingBytes) {
    const Bytes full = signed_by_alice().serialize();
    EXPECT_THROW(Transaction::deserialize(ByteSpan(full).first(full.size() - 1)), DeserializeError);
    Bytes trailing = full;
    trailing.push_back(0);
    EXPECT_THROW(Transaction::deserialize(trailing), DeserializeError);
}

TEST(Transaction, DeserializeEnforcesMemoLimit) {
    Transaction tx;
    tx.outputs = {{1, test_key("alice").public_key()}};
    tx.memo = Bytes(Transaction::kMaxMemo, 'x');
    EXPECT_NO_THROW(Transaction::deserialize(tx.serialize()));
    tx.memo.push_back('x');
    EXPECT_THROW(Transaction::deserialize(tx.serialize()), DeserializeError);
}

TEST(Transaction, DeserializeRejectsImpossibleCounts) {
    // Version 1, then a claim of 4,294,967,295 inputs with no bytes behind it.
    EXPECT_THROW(Transaction::deserialize(from_hex("01000000" "feffffffff").value()), DeserializeError);
    // Version 1, no inputs, then a claim of 4,294,967,295 outputs.
    EXPECT_THROW(Transaction::deserialize(from_hex("01000000" "00" "feffffffff").value()), DeserializeError);
}

TEST(Transaction, VerifyInput) {
    const PublicKey alice = test_key("alice").public_key();
    const PublicKey bob = test_key("bob").public_key();

    Transaction tx = rent_payment();
    EXPECT_FALSE(verify_input(tx, 0, alice));  // not signed yet

    sign_all_inputs(tx, test_key("alice"));
    EXPECT_TRUE(verify_input(tx, 0, alice));
    EXPECT_TRUE(verify_input(tx, 1, alice));
    EXPECT_FALSE(verify_input(tx, 0, bob));
    EXPECT_FALSE(verify_input(tx, 2, alice));  // no such input

    tx.outputs[0].amount += 1;  // changes the sighash, so the signatures no longer match
    EXPECT_FALSE(verify_input(tx, 0, alice));
}

TEST(Transaction, IsCoinbase) {
    EXPECT_FALSE(rent_payment().is_coinbase());
    EXPECT_TRUE(Transaction{}.is_coinbase());
}
