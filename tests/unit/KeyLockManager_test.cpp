#include <KeyCodec.h>
#include <LockManager.h>
#include <LockState.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <utility>

using namespace std::chrono_literals;

TEST(KeyLockManagerTest, MultipleTransactionsCanOwnSharedLock) {
    LockManager manager;
    Key key = KeyCodec::make_string("key");

    EXPECT_EQ(manager.lock_shared(1, key), LockManagerStatus::Success);
    EXPECT_EQ(manager.lock_shared(2, key), LockManagerStatus::Success);
    EXPECT_EQ(manager.lock_shared(1, key), LockManagerStatus::TxnHoldsShared);

    EXPECT_EQ(manager.unlock_shared(1, key), LockManagerStatus::Success);
    EXPECT_EQ(manager.unlock_shared(2, key), LockManagerStatus::Success);
}

TEST(KeyLockManagerTest, ExclusiveOwnerIsReportedAndValidatedOnUnlock) {
    LockManager manager;
    Key key = KeyCodec::make_string("key");

    EXPECT_EQ(manager.lock_exclusive(1, key), LockManagerStatus::Success);
    EXPECT_EQ(
        manager.lock_exclusive(1, key),
        LockManagerStatus::TxnHoldsExclusive
    );
    EXPECT_EQ(manager.lock_shared(1, key), LockManagerStatus::TxnHoldsExclusive);
    EXPECT_EQ(manager.unlock_exclusive(2, key), LockManagerStatus::NotOwner);
    EXPECT_EQ(manager.unlock_exclusive(1, key), LockManagerStatus::Success);
}

TEST(KeyLockManagerTest, SoleSharedOwnerPromotesImmediately) {
    LockManager manager;
    Key key = KeyCodec::make_string("key");

    ASSERT_EQ(manager.lock_shared(1, key), LockManagerStatus::Success);
    EXPECT_EQ(manager.lock_exclusive(1, key), LockManagerStatus::Success);
    EXPECT_EQ(manager.unlock_shared(1, key), LockManagerStatus::NotOwner);
    EXPECT_EQ(manager.unlock_exclusive(1, key), LockManagerStatus::Success);
}

TEST(KeyLockManagerTest, InvalidUnlockDoesNotCreateOrRetainLockState) {
    LockManager manager;
    Key key = KeyCodec::make_string("key");

    EXPECT_EQ(manager.unlock_shared(1, key), LockManagerStatus::NotOwner);
    EXPECT_EQ(manager.unlock_exclusive(1, key), LockManagerStatus::NotOwner);

    EXPECT_EQ(manager.lock_exclusive(2, key), LockManagerStatus::Success);
    EXPECT_EQ(manager.unlock_exclusive(2, key), LockManagerStatus::Success);

    EXPECT_EQ(manager.lock_shared(3, key), LockManagerStatus::Success);
    EXPECT_EQ(manager.unlock_shared(3, key), LockManagerStatus::Success);
}

TEST(KeyLockManagerTest, WriterWaitsUntilEveryReaderReleases) {
    LockManager manager;
    Key key = KeyCodec::make_string("key");

    ASSERT_EQ(manager.lock_shared(1, key), LockManagerStatus::Success);
    ASSERT_EQ(manager.lock_shared(2, key), LockManagerStatus::Success);

    std::promise<void> writer_started;
    std::future<void> started = writer_started.get_future();
    std::future<LockManagerStatus> writer = std::async(
        std::launch::async,
        [&manager, &key, started_signal = std::move(writer_started)]() mutable {
            started_signal.set_value();
            return manager.lock_exclusive(3, key);
        }
    );

    started.wait();
    EXPECT_EQ(writer.wait_for(50ms), std::future_status::timeout);

    EXPECT_EQ(manager.unlock_shared(1, key), LockManagerStatus::Success);
    EXPECT_EQ(writer.wait_for(50ms), std::future_status::timeout);

    EXPECT_EQ(manager.unlock_shared(2, key), LockManagerStatus::Success);
    ASSERT_EQ(writer.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(writer.get(), LockManagerStatus::Success);
    EXPECT_EQ(manager.unlock_exclusive(3, key), LockManagerStatus::Success);
}

TEST(KeyLockManagerTest, ExclusiveReleaseWakesConsecutiveReaders) {
    LockManager manager;
    Key key = KeyCodec::make_string("key");

    ASSERT_EQ(manager.lock_exclusive(1, key), LockManagerStatus::Success);

    std::promise<void> first_started;
    std::future<void> first_start = first_started.get_future();
    std::future<LockManagerStatus> first_reader = std::async(
        std::launch::async,
        [&manager, &key, signal = std::move(first_started)]() mutable {
            signal.set_value();
            return manager.lock_shared(2, key);
        }
    );

    first_start.wait();
    EXPECT_EQ(first_reader.wait_for(50ms), std::future_status::timeout);

    std::promise<void> second_started;
    std::future<void> second_start = second_started.get_future();
    std::future<LockManagerStatus> second_reader = std::async(
        std::launch::async,
        [&manager, &key, signal = std::move(second_started)]() mutable {
            signal.set_value();
            return manager.lock_shared(3, key);
        }
    );

    second_start.wait();
    EXPECT_EQ(second_reader.wait_for(50ms), std::future_status::timeout);

    EXPECT_EQ(manager.unlock_exclusive(1, key), LockManagerStatus::Success);

    ASSERT_EQ(first_reader.wait_for(1s), std::future_status::ready);
    ASSERT_EQ(second_reader.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(first_reader.get(), LockManagerStatus::Success);
    EXPECT_EQ(second_reader.get(), LockManagerStatus::Success);

    EXPECT_EQ(manager.unlock_shared(2, key), LockManagerStatus::Success);
    EXPECT_EQ(manager.unlock_shared(3, key), LockManagerStatus::Success);
}

TEST(KeyLockManagerTest, FrontWriterPreventsReaderFromBypassingQueue) {
    LockState state;
    state.shared_owners.insert(1);

    auto writer = std::make_shared<Waiter>();
    writer->txn_id = 2;
    writer->lock_mode = LockMode::Exclusive;

    auto reader = std::make_shared<Waiter>();
    reader->txn_id = 3;
    reader->lock_mode = LockMode::Shared;

    state.waiters.push_back(writer);
    state.waiters.push_back(reader);

    EXPECT_TRUE(LockState::grant_waiters(state).empty());
    EXPECT_FALSE(writer->granted);
    EXPECT_FALSE(reader->granted);

    state.shared_owners.erase(1);

    EXPECT_EQ(LockState::grant_waiters(state).size(), 1u);
    EXPECT_TRUE(writer->granted);
    EXPECT_FALSE(reader->granted);
    ASSERT_TRUE(state.exclusive_owner.has_value());
    EXPECT_EQ(*state.exclusive_owner, 2);
    ASSERT_FALSE(state.waiters.empty());
    EXPECT_EQ(state.waiters.front()->txn_id, 3);

    state.exclusive_owner.reset();

    EXPECT_EQ(LockState::grant_waiters(state).size(), 1u);
    EXPECT_TRUE(reader->granted);
    EXPECT_TRUE(state.shared_owners.contains(3));
    EXPECT_TRUE(state.waiters.empty());
}

TEST(KeyLockManagerTest, DifferentKeysDoNotConflict) {
    LockManager manager;
    Key first = KeyCodec::make_string("first");
    Key second = KeyCodec::make_string("second");

    EXPECT_EQ(manager.lock_exclusive(1, first), LockManagerStatus::Success);
    EXPECT_EQ(manager.lock_exclusive(2, second), LockManagerStatus::Success);

    EXPECT_EQ(manager.unlock_exclusive(1, first), LockManagerStatus::Success);
    EXPECT_EQ(manager.unlock_exclusive(2, second), LockManagerStatus::Success);
}
