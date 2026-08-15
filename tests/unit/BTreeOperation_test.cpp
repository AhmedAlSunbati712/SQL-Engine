#include <Pager.h>
#include <containers/BTreeOperation.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>

using namespace std::chrono_literals;

TEST(BTreeOperationTest, SharedLatchAllowsAnotherReader) {
    Pager pager;
    PageV2 page;
    page.page_num = 7;
    BTreeOperation operation(1, pager);

    EXPECT_EQ(operation.lock_shared(&page), &page);
    ASSERT_TRUE(operation.latch_mode(page.page_num).has_value());
    EXPECT_EQ(*operation.latch_mode(page.page_num), PageLatchMode::Shared);

    std::future<bool> reader = std::async(std::launch::async, [&] {
        const bool acquired = page.latch.try_lock_shared();
        if (acquired) page.latch.unlock_shared();
        return acquired;
    });

    EXPECT_TRUE(reader.get());
    operation.release(page.page_num);
    EXPECT_FALSE(operation.latch_mode(page.page_num).has_value());
}

TEST(BTreeOperationTest, ExclusiveLatchBlocksReaderUntilRelease) {
    Pager pager;
    PageV2 page;
    page.page_num = 8;
    BTreeOperation operation(1, pager);

    ASSERT_EQ(operation.lock_exclusive(&page), &page);
    ASSERT_TRUE(operation.latch_mode(page.page_num).has_value());
    EXPECT_EQ(*operation.latch_mode(page.page_num), PageLatchMode::Exclusive);

    std::promise<void> reader_started;
    std::future<void> started = reader_started.get_future();
    std::future<void> reader = std::async(
        std::launch::async,
        [&page, signal = std::move(reader_started)]() mutable {
            signal.set_value();
            std::shared_lock lock(page.latch);
        });

    started.wait();
    EXPECT_EQ(reader.wait_for(50ms), std::future_status::timeout);

    operation.release(page.page_num);
    EXPECT_EQ(reader.wait_for(1s), std::future_status::ready);
    reader.get();
}

TEST(BTreeOperationTest, DestructorReleasesEveryHeldLatch) {
    Pager pager;
    PageV2 first;
    PageV2 second;
    first.page_num = 9;
    second.page_num = 10;

    {
        BTreeOperation operation(1, pager);
        ASSERT_EQ(operation.lock_exclusive(&first), &first);
        ASSERT_EQ(operation.lock_shared(&second), &second);
    }

    EXPECT_TRUE(first.latch.try_lock());
    first.latch.unlock();
    EXPECT_TRUE(second.latch.try_lock());
    second.latch.unlock();
}

TEST(BTreeOperationTest, RejectsNullAndDuplicatePages) {
    Pager pager;
    PageV2 page;
    page.page_num = 11;
    BTreeOperation operation(1, pager);

    EXPECT_THROW(operation.lock_shared(nullptr), std::invalid_argument);
    ASSERT_EQ(operation.lock_shared(&page), &page);
    EXPECT_THROW(operation.lock_exclusive(&page), std::logic_error);
}
