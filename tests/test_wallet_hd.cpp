#include "core/mempool.h"
#include "core/storage.h"
#include "core/validation.h"
#include "crypto/sha256.h"
#include "test_util.h"
#include "util/serialize.h"
#include "wallet/crypter.h"
#include "wallet/wallet.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

using namespace bound;

TEST(Crypter, RoundTrip) {
    const Bytes secret{1, 2, 3, 4, 5};
    const Bytes ad{'a', 'd'};
    const Bytes blob = encrypt_secret(secret, "correct horse", ad, kTestKdf);
    EXPECT_EQ(blob.size(), 16u + 8 + 8 + 24 + 1 + secret.size() + 16);
    EXPECT_EQ(decrypt_secret(blob, "correct horse", ad), secret);
    EXPECT_EQ(decrypt_secret(blob, "correct horsf", ad), std::nullopt);
    EXPECT_EQ(decrypt_secret(blob, "correct horse", Bytes{'a', 'e'}), std::nullopt);
    Bytes tampered = blob;
    tampered.back() ^= 1;
    EXPECT_EQ(decrypt_secret(tampered, "correct horse", ad), std::nullopt);
    // A fresh salt and nonce every time.
    EXPECT_NE(encrypt_secret(secret, "correct horse", ad, kTestKdf), blob);
    EXPECT_THROW(decrypt_secret(Bytes(10), "x", ad), DeserializeError);
}

TEST(WalletKeys, DerivationIsDeterministicPerBranchAndIndex) {
    const Seed seed = sha256(bytes_of("wallet test seed"));
    ByteWriter w;
    w.bytes(seed);
    w.u32(1);
    w.u32(7);
    const KeyPair expected = KeyPair::from_seed(tagged_hash("BOUND/wallet-key", w.data()));
    EXPECT_EQ(derive_key(seed, 1, 7).public_key(), expected.public_key());
    EXPECT_NE(derive_key(seed, 0, 7).public_key(), expected.public_key());
    EXPECT_NE(derive_key(seed, 1, 8).public_key(), expected.public_key());
    EXPECT_NE(random_seed(), random_seed());
}

namespace {

class HdWalletTest : public ::testing::Test {
protected:
    HdWalletTest() { wallet = make(db); }

    std::unique_ptr<Wallet> make(KeyValueStore& store, bool rescan = false) {
        return Wallet::create(store, chain, mempool, "pass", seed, rescan, options());
    }
    static WalletOptions options() {
        WalletOptions o;
        o.lookahead = 5;
        o.kdf = kTestKdf;
        return o;
    }
    // Mines `count` blocks paying `to` (with whatever the mempool holds in the first).
    void mine(const PublicKey& to, int count = 1) {
        for (int i = 0; i < count; ++i) {
            ASSERT_TRUE(chain.accept_block(next_block(chain, to, mempool.select(1'000'000).txs), kTestNow).accepted());
        }
    }
    void unlock() { wallet->unlock("pass", kTestNow + 1000); }
    // Mines 101 blocks, the first to the wallet: 5 BOUND it can spend.
    PublicKey fund() {
        const PublicKey mine_to = wallet->new_address();
        mine(mine_to);
        mine(stranger, 100);
        return mine_to;
    }

    const ConsensusParams params = easy_params();
    const Seed seed = sha256(bytes_of("wallet test seed"));
    const PublicKey stranger = test_key("stranger").public_key();
    const PublicKey bob = test_key("bob").public_key();
    Chainstate chain{params};
    Mempool mempool{chain};
    MemoryStore db;
    std::unique_ptr<Wallet> wallet;
};

}  // namespace

TEST_F(HdWalletTest, ANewWalletIsLockedWithKeysReady) {
    EXPECT_TRUE(wallet->locked());
    EXPECT_EQ(wallet->keypool_size(), 10u);  // 5 per branch
    EXPECT_THROW(wallet->seed(), WalletError);
    // Addresses come from the seed, in order, even while locked.
    EXPECT_EQ(wallet->new_address(), derive_key(seed, kReceiveBranch, 0).public_key());
    EXPECT_EQ(wallet->new_address(), derive_key(seed, kReceiveBranch, 1).public_key());
    EXPECT_TRUE(wallet->is_mine(derive_key(seed, kChangeBranch, 4).public_key()));
    EXPECT_FALSE(wallet->is_mine(bob));
    EXPECT_TRUE(Wallet::exists(db));
    EXPECT_THROW(make(db), std::runtime_error);  // one wallet per store
}

TEST_F(HdWalletTest, ThePassphraseUnlocksForAWhile) {
    try {
        wallet->unlock("wrong", kTestNow + 60);
        FAIL() << "unlocked with the wrong passphrase";
    } catch (const WalletError& e) {
        EXPECT_EQ(e.kind, WalletError::Kind::WrongPassphrase);
    }
    wallet->unlock("pass", kTestNow + 60);
    EXPECT_FALSE(wallet->locked());
    EXPECT_EQ(wallet->seed(), seed);
    EXPECT_EQ(wallet->unlocked_until(), kTestNow + 60);
    wallet->tick(kTestNow + 59);
    EXPECT_FALSE(wallet->locked());
    wallet->tick(kTestNow + 60);
    EXPECT_TRUE(wallet->locked());

    wallet->change_passphrase("pass", "new pass");
    EXPECT_THROW(wallet->unlock("pass", kTestNow + 60), WalletError);
    wallet->unlock("new pass", kTestNow + 60);
    EXPECT_EQ(wallet->seed(), seed);
}

TEST_F(HdWalletTest, ALockedWalletRunsOutOfAddresses) {
    for (int i = 0; i < 5; ++i) wallet->new_address();
    try {
        wallet->new_address();
        FAIL() << "a sixth address while locked";
    } catch (const WalletError& e) {
        EXPECT_EQ(e.kind, WalletError::Kind::KeypoolEmpty);
    }
    unlock();  // tops the keypool up
    EXPECT_EQ(wallet->new_address(), derive_key(seed, kReceiveBranch, 5).public_key());
}

TEST_F(HdWalletTest, MinedCoinsMatureAfterAHundredBlocks) {
    const PublicKey to = wallet->new_address();
    mine(to);
    EXPECT_EQ(wallet->balance().immature, 5 * kCoin);
    EXPECT_EQ(wallet->balance().spendable, 0);
    mine(stranger, 98);
    EXPECT_EQ(wallet->balance().immature, 5 * kCoin);  // at #99, the next block can't spend it yet
    mine(stranger, 1);
    EXPECT_EQ(wallet->balance().spendable, 5 * kCoin);  // at #100, block #101 can
    EXPECT_EQ(wallet->balance().immature, 0);
    const std::vector<WalletTxSummary> history = wallet->transactions();
    ASSERT_EQ(history.size(), 1u);
    EXPECT_TRUE(history[0].coinbase);
    EXPECT_EQ(history[0].received, 5 * kCoin);
    EXPECT_EQ(history[0].confirmations, 100);
}

TEST_F(HdWalletTest, PayingSomeone) {
    fund();
    try {
        wallet->create_payment(bob, kCoin, FeeRate{1000});
        FAIL() << "paid while locked";
    } catch (const WalletError& e) {
        EXPECT_EQ(e.kind, WalletError::Kind::Locked);
    }
    unlock();
    const Transaction tx = wallet->create_payment(bob, 2 * kCoin, FeeRate{2000}, bytes_of("rent"));
    ASSERT_EQ(tx.outputs.size(), 2u);
    ASSERT_EQ(tx.serialize().size(), 195u);  // 1 in, 2 out, 4-byte memo
    const Amount fee = 390;                  // 2 units per byte
    const auto to_bob = std::find_if(tx.outputs.begin(), tx.outputs.end(), [&](const TxOut& o) { return o.recipient == bob; });
    ASSERT_NE(to_bob, tx.outputs.end());
    EXPECT_EQ(to_bob->amount, 2 * kCoin);
    const TxOut& change = tx.outputs[to_bob == tx.outputs.begin() ? 1 : 0];
    EXPECT_EQ(change.amount, 3 * kCoin - fee);
    EXPECT_EQ(change.recipient, derive_key(seed, kChangeBranch, 0).public_key());

    // Nothing is recorded until the mempool takes it.
    EXPECT_EQ(wallet->transaction_count(), 1u);
    ASSERT_TRUE(mempool.accept(tx, kTestNow).accepted());
    EXPECT_EQ(wallet->transaction_count(), 2u);
    EXPECT_EQ(wallet->balance().spendable, 3 * kCoin - fee);  // the change counts at once: it's ours
    const WalletTxSummary sent = wallet->summarize(*wallet->find(tx.txid()));
    EXPECT_TRUE(sent.from_me);
    EXPECT_EQ(sent.fee, fee);
    EXPECT_EQ(sent.sent, 5 * kCoin);
    EXPECT_EQ(sent.received, 3 * kCoin - fee);
    EXPECT_EQ(sent.confirmations, 0);

    mine(stranger);
    EXPECT_EQ(wallet->confirmations(*wallet->find(tx.txid())), 1);
    EXPECT_EQ(wallet->balance().spendable, 3 * kCoin - fee);
    EXPECT_EQ(wallet->coins().size(), 1u);
    EXPECT_TRUE(wallet->coins()[0].change);
}

TEST_F(HdWalletTest, SpendingUnconfirmedChange) {
    fund();
    unlock();
    const Transaction first = wallet->create_payment(bob, kCoin, FeeRate{1000});
    ASSERT_TRUE(mempool.accept(first, kTestNow).accepted());
    const Transaction second = wallet->create_payment(bob, kCoin, FeeRate{1000});
    ASSERT_EQ(second.inputs.size(), 1u);
    EXPECT_EQ(second.inputs[0].txid, first.txid());  // the only coin left is first's change
    ASSERT_TRUE(mempool.accept(second, kTestNow).accepted());
    mine(stranger);
    EXPECT_EQ(mempool.size(), 0u);
    EXPECT_EQ(wallet->balance().spendable, 3 * kCoin - 191 - 191);
}

TEST_F(HdWalletTest, NotEnoughMoney) {
    fund();
    unlock();
    try {
        wallet->create_payment(bob, 5 * kCoin, FeeRate{1000});  // the fee doesn't fit
        FAIL();
    } catch (const WalletError& e) {
        EXPECT_EQ(e.kind, WalletError::Kind::InsufficientFunds);
    }
    EXPECT_THROW(wallet->create_payment(bob, 419, FeeRate{1000}), WalletError);  // dust
    // Change that would be dust goes to the miner instead.
    const Transaction all = wallet->create_payment(bob, 5 * kCoin - 151 - 100, FeeRate{1000});
    ASSERT_EQ(all.outputs.size(), 1u);
    EXPECT_TRUE(mempool.accept(all, kTestNow).accepted());
    EXPECT_EQ(mempool.find(all.txid())->fee, 251);
}

// Milestone 18: the wallet app's Max sends every coin, the fee taken out of the amount.
TEST_F(HdWalletTest, SendingEverything) {
    mine(wallet->new_address());
    mine(wallet->new_address());
    mine(stranger, 100);
    unlock();
    EXPECT_EQ(wallet->balance().spendable, 10 * kCoin);
    const Transaction tx = wallet->create_sweep(bob, FeeRate{1000}, bytes_of("all"));
    ASSERT_EQ(tx.inputs.size(), 2u);
    ASSERT_EQ(tx.outputs.size(), 1u);
    ASSERT_EQ(tx.serialize().size(), 254u);  // 2 in, 1 out, a 3-byte memo
    EXPECT_EQ(tx.outputs[0].recipient, bob);
    EXPECT_EQ(tx.outputs[0].amount, 10 * kCoin - 254);
    ASSERT_TRUE(mempool.accept(tx, kTestNow).accepted());
    EXPECT_EQ(mempool.find(tx.txid())->fee, 254);
    EXPECT_EQ(wallet->balance().spendable, 0);
    try {
        wallet->create_sweep(bob, FeeRate{1000});
        FAIL() << "swept an empty wallet";
    } catch (const WalletError& e) {
        EXPECT_EQ(e.kind, WalletError::Kind::InsufficientFunds);
    }
}

TEST_F(HdWalletTest, IncomingPaymentsShowUnconfirmedThenConfirmed) {
    const KeyPair alice = test_key("alice");
    mine(alice.public_key());
    mine(stranger, 100);
    const PublicKey to = wallet->new_address();
    const Transaction pay = spend({coinbase_at(chain, 1)}, {{2 * kCoin, to}, {3 * kCoin - 1000, alice.public_key()}}, alice);
    ASSERT_TRUE(mempool.accept(pay, kTestNow).accepted());
    EXPECT_EQ(wallet->balance().unconfirmed, 2 * kCoin);
    EXPECT_EQ(wallet->balance().spendable, 0);  // someone else's unconfirmed payment can still vanish
    mine(stranger);
    EXPECT_EQ(wallet->balance().spendable, 2 * kCoin);
    EXPECT_EQ(wallet->balance().unconfirmed, 0);
    const WalletTxSummary got = wallet->summarize(*wallet->find(pay.txid()));
    EXPECT_FALSE(got.from_me);
    EXPECT_EQ(got.received, 2 * kCoin);
}

TEST_F(HdWalletTest, BumpingTheFee) {
    fund();
    unlock();
    const Transaction original = wallet->create_payment(bob, kCoin, FeeRate{1000});
    ASSERT_TRUE(mempool.accept(original, kTestNow).accepted());
    const Transaction bumped = wallet->create_bump(original.txid(), FeeRate{10000});
    EXPECT_EQ(bumped.inputs, original.inputs);
    const MempoolAccept r = mempool.accept(bumped, kTestNow);
    ASSERT_TRUE(r.accepted()) << r.reason;
    EXPECT_EQ(r.fee, 1910);
    EXPECT_EQ(r.replaced, std::vector<uint256>{original.txid()});
    EXPECT_EQ(wallet->status(*wallet->find(original.txid())), TxStatus::Inactive);
    EXPECT_EQ(wallet->balance().spendable, 4 * kCoin - 1910);
    EXPECT_TRUE(wallet->to_resubmit().empty());  // its coin is spent by the replacement

    // A replacement has to beat the original by the relay fee at least, whatever rate was asked for.
    const Transaction again = wallet->create_bump(bumped.txid(), FeeRate{0});
    EXPECT_TRUE(mempool.accept(again, kTestNow).accepted());
    mine(stranger);
    try {
        wallet->create_bump(again.txid(), FeeRate{20000});
        FAIL() << "bumped a confirmed transaction";
    } catch (const WalletError& e) {
        EXPECT_EQ(e.kind, WalletError::Kind::Invalid);
    }
}

TEST_F(HdWalletTest, BumpingAddsCoinsWhenTheChangeIsntEnough) {
    fund();
    mine(wallet->new_address());
    mine(stranger, 100);  // a second 5 BOUND coin, mature
    unlock();
    const Transaction original = wallet->create_payment(bob, 5 * kCoin - 1000, FeeRate{1000});
    ASSERT_EQ(original.inputs.size(), 1u);
    ASSERT_EQ(original.outputs.size(), 2u);  // 809 change
    ASSERT_TRUE(mempool.accept(original, kTestNow).accepted());
    const Transaction bumped = wallet->create_bump(original.txid(), FeeRate{50000});
    EXPECT_EQ(bumped.inputs.size(), 2u);
    EXPECT_TRUE(mempool.accept(bumped, kTestNow).accepted());
}

TEST_F(HdWalletTest, ItRemembersEverythingAcrossRestarts) {
    fund();
    unlock();
    const Transaction tx = wallet->create_payment(bob, kCoin, FeeRate{1000});
    ASSERT_TRUE(mempool.accept(tx, kTestNow).accepted());
    mine(stranger);
    const WalletBalance before = wallet->balance();
    wallet.reset();

    mine(stranger, 3);  // while the wallet is closed
    wallet = Wallet::load(db, chain, mempool, options());
    EXPECT_TRUE(wallet->locked());
    EXPECT_EQ(wallet->balance().spendable, before.spendable);
    EXPECT_EQ(wallet->transaction_count(), 2u);
    EXPECT_EQ(wallet->confirmations(*wallet->find(tx.txid())), 4);
    EXPECT_EQ(wallet->new_address(), derive_key(seed, kReceiveBranch, 1).public_key());  // carries on
    wallet->unlock("pass", kTestNow + 100);
}

TEST_F(HdWalletTest, ItCatchesUpWithBlocksMissedWhileClosed) {
    const PublicKey to = wallet->new_address();
    wallet.reset();
    mine(to);
    mine(stranger, 100);
    wallet = Wallet::load(db, chain, mempool, options());
    EXPECT_EQ(wallet->balance().spendable, 5 * kCoin);
}

TEST_F(HdWalletTest, RestoringFromTheSeedFindsTheHistory) {
    fund();
    unlock();
    for (int i = 0; i < 8; ++i) mine(wallet->new_address());  // past the 5-key lookahead
    mine(stranger, 100);
    const Amount total = wallet->balance().spendable;
    ASSERT_EQ(total, 9 * 5 * kCoin);

    MemoryStore fresh;
    const std::unique_ptr<Wallet> restored = make(fresh, true);
    EXPECT_EQ(restored->balance().spendable, total);
    EXPECT_EQ(restored->transaction_count(), 9u);
}

TEST_F(HdWalletTest, AReorganisationUndoesAndRedoesConfirmations) {
    const PublicKey to = wallet->new_address();
    const BlockIndex& fork = chain.tip();
    mine(to);
    EXPECT_EQ(wallet->balance().immature, 5 * kCoin);
    // A longer rival branch without our block.
    const Block r1 = block_on(fork, params, stranger, kTestGenesisTime + 61);
    ASSERT_TRUE(chain.accept_block(r1, kTestNow).accepted());
    ASSERT_TRUE(chain.accept_block(block_on(*chain.find(r1.hash()), params, stranger, kTestGenesisTime + 121), kTestNow).accepted());
    ASSERT_EQ(chain.height(), 2);
    EXPECT_EQ(wallet->balance().immature, 0);
    ASSERT_EQ(wallet->transactions().size(), 1u);
    EXPECT_EQ(wallet->transactions()[0].confirmations, -1);  // an orphaned coinbase
}

TEST_F(HdWalletTest, LostTransactionsAreResubmitted) {
    fund();
    unlock();
    const Transaction tx = wallet->create_payment(bob, kCoin, FeeRate{1000});
    ASSERT_TRUE(mempool.accept(tx, kTestNow).accepted());
    ASSERT_EQ(mempool.expire(kTestNow + 15 * 24 * 3600), 1u);  // the mempool drops it
    EXPECT_EQ(wallet->status(*wallet->find(tx.txid())), TxStatus::Inactive);
    EXPECT_EQ(wallet->balance().spendable, 5 * kCoin);  // the coin is free again
    const std::vector<Transaction> again = wallet->to_resubmit();
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].txid(), tx.txid());
    EXPECT_TRUE(mempool.accept(again[0], kTestNow).accepted());
    EXPECT_TRUE(wallet->to_resubmit().empty());
}
