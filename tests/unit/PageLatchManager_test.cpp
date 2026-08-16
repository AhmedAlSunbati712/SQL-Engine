#include <PageLatchManager.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <type_traits>

using namespace std::chrono_literals;

TEST(PageLatchManagerTest, SharedLatchesCanBeHeldTogether) {
    PageLatchManager manager;
    PageReadLatch first = manager.lock_shared(7);

    std::future<PageReadLatch> second = std::async(
        std::launch::async,
        [&] { return manager.lock_shared(7); });

    ASSERT_EQ(second.wait_for(1s), std::future_status::ready);
    PageReadLatch acquired = second.get();
    EXPECT_TRUE(first.owns_lock());
    EXPECT_TRUE(acquired.owns_lock());
}

TEST(PageLatchManagerTest, ExclusiveLatchBlocksReadersUntilReleased) {
    PageLatchManager manager;
    std::optional<PageWriteLatch> writer(manager.lock_exclusive(8));

    std::future<PageReadLatch> reader = std::async(
        std::launch::async,
        [&] { return manager.lock_shared(8); });

    EXPECT_EQ(reader.wait_for(50ms), std::future_status::timeout);
    writer.reset();
    EXPECT_EQ(reader.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(reader.get().owns_lock());
}

TEST(PageLatchManagerTest, DifferentPagesDoNotBlockEachOther) {
    PageLatchManager manager;
    PageWriteLatch first = manager.lock_exclusive(9);

    std::future<PageWriteLatch> second = std::async(
        std::launch::async,
        [&] { return manager.lock_exclusive(10); });

    ASSERT_EQ(second.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(first.owns_lock());
    EXPECT_TRUE(second.get().owns_lock());
}

TEST(PageLatchManagerTest, GuardsAreMoveOnlyAndPreserveOwnership) {
    static_assert(!std::is_copy_constructible_v<PageReadLatch>);
    static_assert(!std::is_copy_assignable_v<PageReadLatch>);
    static_assert(!std::is_copy_constructible_v<PageWriteLatch>);
    static_assert(!std::is_copy_assignable_v<PageWriteLatch>);

    PageLatchManager manager;
    PageWriteLatch original = manager.lock_exclusive(11);
    PageWriteLatch moved = std::move(original);

    EXPECT_EQ(moved.page_num(), 11U);
    EXPECT_TRUE(moved.owns_lock());
}
