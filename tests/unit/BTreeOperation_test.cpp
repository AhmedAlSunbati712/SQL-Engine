#include <PageLatchManager.h>
#include <containers/BTreeOperation.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>

using namespace std::chrono_literals;

TEST(BTreeOperationTest, SharedLatchAllowsAnotherReader) {
    PageLatchManager manager;
    BTreeOperation operation(manager);

    operation.lock_shared(7);
    ASSERT_TRUE(operation.latch_mode(7).has_value());
    EXPECT_EQ(*operation.latch_mode(7), PageLatchMode::Shared);

    std::future<PageReadLatch> reader = std::async(
        std::launch::async,
        [&] { return manager.lock_shared(7); });

    EXPECT_EQ(reader.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(reader.get().owns_lock());
    operation.release(7);
    EXPECT_FALSE(operation.latch_mode(7).has_value());
}

TEST(BTreeOperationTest, ExclusiveLatchBlocksReaderUntilRelease) {
    PageLatchManager manager;
    BTreeOperation operation(manager);

    operation.lock_exclusive(8);
    ASSERT_TRUE(operation.latch_mode(8).has_value());
    EXPECT_EQ(*operation.latch_mode(8), PageLatchMode::Exclusive);

    std::future<PageReadLatch> reader = std::async(
        std::launch::async,
        [&] { return manager.lock_shared(8); });

    EXPECT_EQ(reader.wait_for(50ms), std::future_status::timeout);
    operation.release(8);
    EXPECT_EQ(reader.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(reader.get().owns_lock());
}

TEST(BTreeOperationTest, DestructorReleasesEveryHeldLatch) {
    PageLatchManager manager;

    {
        BTreeOperation operation(manager);
        operation.lock_exclusive(9);
        operation.lock_shared(10);
    }

    EXPECT_TRUE(manager.lock_exclusive(9).owns_lock());
    EXPECT_TRUE(manager.lock_exclusive(10).owns_lock());
}

TEST(BTreeOperationTest, DuplicateRequestsAreIdempotentAndUpgradeIsRejected) {
    PageLatchManager manager;
    BTreeOperation operation(manager);

    operation.lock_shared(11);
    EXPECT_NO_THROW(operation.lock_shared(11));
    EXPECT_THROW(operation.lock_exclusive(11), std::logic_error);

    operation.lock_exclusive(12);
    EXPECT_NO_THROW(operation.lock_exclusive(12));
    EXPECT_NO_THROW(operation.lock_shared(12));
    EXPECT_EQ(*operation.latch_mode(12), PageLatchMode::Exclusive);
}

TEST(BTreeOperationTest, ReleasesExclusiveAncestorsButKeepsRequestedPage) {
    PageLatchManager manager;
    BTreeOperation operation(manager);

    operation.lock_exclusive(0);
    operation.lock_exclusive(4);
    operation.lock_shared(8);
    operation.lock_exclusive(12);

    operation.release_all_exclusive_except(12);

    EXPECT_FALSE(operation.latch_mode(0).has_value());
    EXPECT_FALSE(operation.latch_mode(4).has_value());
    ASSERT_TRUE(operation.latch_mode(8).has_value());
    EXPECT_EQ(*operation.latch_mode(8), PageLatchMode::Shared);
    ASSERT_TRUE(operation.latch_mode(12).has_value());
    EXPECT_EQ(*operation.latch_mode(12), PageLatchMode::Exclusive);
}
