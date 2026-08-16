#include <gtest/gtest.h>
#include <PCache.h>

#include <future>
#include <vector>

namespace {

PageV2 *make_page(int page_num, int refs_num = 0, bool is_dirty = false, bool need_flushing = false) {
    PageV2 *page = new PageV2();
    page->page_num = page_num;
    page->refs_num = refs_num;
    page->is_dirty = is_dirty;
    page->need_flushing = need_flushing;
    return page;
}

TEST(PCacheTest, StartsEmpty) {
    PCache cache;
    EXPECT_EQ(cache.len(), 0);
}

TEST(PCacheTest, GetMissingPageReturnsNull) {
    PCache cache;
    PageV2 *page = cache.get(3);
    EXPECT_EQ(page, nullptr);
}

TEST(PCacheTest, PutAddsPageToCache) {
    PCache cache;
    PageV2 *page = make_page(4);

    PCachePutResult put_res = cache.put(page);

    EXPECT_EQ(put_res.status, PCacheResult::Success);
    EXPECT_EQ(cache.len(), 1);
    EXPECT_EQ(cache.get(4), page);
    EXPECT_EQ(put_res.eviction.happened, false);

    cache.remove(4);
}

TEST(PCacheTest, RemoveMissingPageIsSuccess) {
    PCache cache;
    int before_len = cache.len();

    PCacheResult remove_res = cache.remove(3);
    int after_len = cache.len();

    EXPECT_EQ(remove_res, PCacheResult::Success);
    EXPECT_EQ(before_len, after_len);
}

TEST(PCacheTest, PutZeroRefPageMakesItImmediatelyRemovable) {
    PCache cache;
    PageV2 *page = make_page(4);

    PCachePutResult put_res = cache.put(page);
    PCacheResult remove_res = cache.remove(4);

    EXPECT_EQ(put_res.status, PCacheResult::Success);
    EXPECT_EQ(remove_res, PCacheResult::Success);
    EXPECT_EQ(cache.len(), 0);
    EXPECT_EQ(cache.get(4), nullptr);
}

TEST(PCacheTest, RemoveExistingUnpinnedPage) {
    PCache cache;
    PageV2 *page = make_page(4);

    PCachePutResult put_res = cache.put(page);
    PCacheResult remove_res = cache.remove(4);

    EXPECT_EQ(put_res.status, PCacheResult::Success);
    EXPECT_EQ(remove_res, PCacheResult::Success);
    EXPECT_EQ(cache.len(), 0);
}

TEST(PCacheTest, RemovePinnedPageReturnsRemovingPinnedPage) {
    PCache cache;
    PageV2 *page = make_page(4, 1);

    PCachePutResult put_res = cache.put(page);
    PCacheResult remove_res = cache.remove(4);

    EXPECT_EQ(put_res.status, PCacheResult::Success);
    EXPECT_EQ(remove_res, PCacheResult::RemovingPinnedPage);
    EXPECT_EQ(cache.len(), 1);
    EXPECT_EQ(cache.get(4), page);
}

TEST(PCacheTest, ForceRemovePinnedPageDropsItFromCache) {
    PCache cache;
    PageV2 *page = make_page(4, 1);

    EXPECT_EQ(cache.put(page).status, PCacheResult::Success);

    cache.force_remove(4);

    EXPECT_EQ(cache.len(), 0);
    EXPECT_EQ(cache.get(4), nullptr);
}

TEST(PCacheTest, PinPageRemovesZeroRefPageFromEvictableSet) {
    PCache cache(1);
    PageV2 *page = make_page(1);
    PageV2 *new_page = make_page(2);

    EXPECT_EQ(cache.put(page).status, PCacheResult::Success);

    page->refs_num = 1;
    cache.pin_page(1);

    PCachePutResult put_res = cache.put(new_page);

    EXPECT_EQ(put_res.status, PCacheResult::NoVictim);
    EXPECT_EQ(cache.len(), 1);
    EXPECT_EQ(cache.get(1), page);

    delete new_page;
}

TEST(PCacheTest, UnpinPageMakesPinnedPageEvictableAgain) {
    PCache cache(1);
    PageV2 *page = make_page(1, 1);
    PageV2 *new_page = make_page(2);

    EXPECT_EQ(cache.put(page).status, PCacheResult::Success);

    page->refs_num = 0;
    cache.unpin_page(1);

    PCachePutResult put_res = cache.put(new_page);

    EXPECT_EQ(put_res.status, PCacheResult::Success);
    EXPECT_EQ(put_res.eviction.happened, true);
    EXPECT_EQ(put_res.eviction.page_num, 1);
    EXPECT_EQ(put_res.eviction.was_dirty, false);
    EXPECT_EQ(cache.get(1), nullptr);
    EXPECT_EQ(cache.get(2), new_page);
}

TEST(PCacheTest, PutReturnsNoVictimWhenFullAndOnlyPageIsPinned) {
    PCache cache(1);
    PageV2 *page = make_page(1, 1);
    PageV2 *new_page = make_page(2);

    EXPECT_EQ(cache.put(page).status, PCacheResult::Success);

    PCachePutResult put_res = cache.put(new_page);

    EXPECT_EQ(put_res.status, PCacheResult::NoVictim);
    EXPECT_EQ(cache.get(1), page);
    EXPECT_EQ(cache.get(2), nullptr);

    delete new_page;
}

TEST(PCacheTest, PutEvictsCleanZeroRefPageWhenFull) {
    PCache cache(1);
    PageV2 *page = make_page(1);
    PageV2 *new_page = make_page(2);

    EXPECT_EQ(cache.put(page).status, PCacheResult::Success);

    PCachePutResult put_res = cache.put(new_page);

    EXPECT_EQ(put_res.status, PCacheResult::Success);
    EXPECT_EQ(put_res.eviction.happened, true);
    EXPECT_EQ(put_res.eviction.page_num, 1);
    EXPECT_EQ(put_res.eviction.was_dirty, false);
    EXPECT_EQ(cache.get(1), nullptr);
    EXPECT_EQ(cache.get(2), new_page);
}

TEST(PCacheTest, PutReturnsDirtyFlushWhenOnlyVictimIsDirtyAndNeedsFlushing) {
    PCache cache(1);
    PageV2 *page = make_page(1, 0, true, true);
    PageV2 *new_page = make_page(2);

    EXPECT_EQ(cache.put(page).status, PCacheResult::Success);

    PCachePutResult put_res = cache.put(new_page);

    EXPECT_EQ(put_res.status, PCacheResult::DirtyFlush);
    EXPECT_EQ(cache.get(1), page);
    EXPECT_EQ(cache.get(2), nullptr);

    delete new_page;
}

TEST(PCacheTest, PutEvictsDirtyPageWhenVictimDoesNotNeedFlushing) {
    PCache cache(1);
    PageV2 *page = make_page(1, 0, true, false);
    PageV2 *new_page = make_page(2);

    EXPECT_EQ(cache.put(page).status, PCacheResult::Success);

    PCachePutResult put_res = cache.put(new_page);

    EXPECT_EQ(put_res.status, PCacheResult::Success);
    EXPECT_EQ(put_res.eviction.happened, true);
    EXPECT_EQ(put_res.eviction.page_num, 1);
    EXPECT_EQ(put_res.eviction.was_dirty, true);
    EXPECT_EQ(cache.get(1), nullptr);
    EXPECT_EQ(cache.get(2), new_page);
}

TEST(PCacheTest, PutPrefersCleanVictimOverDirtyVictim) {
    PCache cache(2);
    PageV2 *dirty_page = make_page(1, 0, true, false);
    PageV2 *clean_page = make_page(2);
    PageV2 *new_page = make_page(3);

    EXPECT_EQ(cache.put(dirty_page).status, PCacheResult::Success);
    EXPECT_EQ(cache.put(clean_page).status, PCacheResult::Success);

    PCachePutResult put_res = cache.put(new_page);

    EXPECT_EQ(put_res.status, PCacheResult::Success);
    EXPECT_EQ(put_res.eviction.happened, true);
    EXPECT_EQ(put_res.eviction.page_num, 2);
    EXPECT_EQ(put_res.eviction.was_dirty, false);
    EXPECT_EQ(cache.get(1), dirty_page);
    EXPECT_EQ(cache.get(2), nullptr);
    EXPECT_EQ(cache.get(3), new_page);
}

TEST(PCacheTest, ConcurrentPutsKeepMetadataConsistent) {
    PCache cache(64);
    std::vector<std::future<PCacheResult>> puts;

    for (int page_num = 1; page_num <= 32; page_num++) {
        puts.push_back(std::async(std::launch::async, [&, page_num] {
            return cache.put(make_page(page_num)).status;
        }));
    }

    for (std::future<PCacheResult>& put : puts) {
        EXPECT_EQ(put.get(), PCacheResult::Success);
    }
    EXPECT_EQ(cache.len(), 32);
    EXPECT_EQ(cache.unpinned_len(), 32);

    for (int page_num = 1; page_num <= 32; page_num++) {
        EXPECT_NE(cache.get(page_num), nullptr);
    }
}

} // namespace
