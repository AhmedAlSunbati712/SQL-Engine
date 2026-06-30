#include <PCache.h>
#include <cassert>

PCache::PCache() {};
PCache::PCache(int capacity): capacity(capacity) {};
PCache::~PCache() {
    /**
     * Free up all the resources used by page structs in the cache by iterating through cache map
     * Free up the DLList too and its nodes. The dllist deconstructor will handle removing the nodes
     */
    auto iterator = cache_map.begin();
    while (iterator != cache_map.end()) {
        delete iterator->second;
        iterator = cache_map.erase(iterator);
    }

    // Free up the unpinned pages list
    delete unpinned_pages;

}

/**
 * @brief looksup a page by its number in our map. If it doesnt, we return a nullptr.
 *        Otherwse, we return a pointer to the page struct.
 */
Page *PCache::get(int page_num) {
    auto iterator = cache_map.find(page_num);
    if (iterator == cache_map.end()) return nullptr;
    return iterator->second;
}

PCachePutResult PCache::put(Page *page) {
    assert(page != nullptr);
    int page_num = page->page_num;

    // Check if the page already exists. If it does, return a status code.
    // Should we just try to replace the page in the future instead?
    auto iterator = cache_map.find(page_num);
    assert(iterator == cache_map.end());

    PCachePutResult result;
    
    // If The cache is full, find a victim unpinned page to evict and evict it
    if (length == capacity) {
        // No victim page to evict? return an error. Later on we will expand the cache temporarily
        if (unpinned_pages->len() == 0) {
            result.status = PCacheResult::NoVictim;
            return result;
        }

        auto it = unpinned_pages->end(); // this returns an iterator that starts at tail
        it--; // Move to the most recently added node. This now is pointing to tail->prev

        // We need to stop when we pass the least recently added node
        auto it_head = unpinned_pages->begin(); // this returns an iterator that starts at head->next
        it_head--; // This is now an iterator that starts at head

        Page *victim_page = nullptr;
        while (it != it_head) {
            Page *candidate_page = *it;
            if (!candidate_page->is_dirty) {
                victim_page = candidate_page;
                break;
            }
            if (!victim_page) victim_page = candidate_page;
            it--;
        }

        // If a victim page page is dirty, we need to do some cleanup
        assert(victim_page != nullptr);
        // Since we added the flag need_flush, what should be the condition we check here?
        // if victim_page->is_dirty + need_flush, we definitely need a cache spillover
        // if need_flush is false, then that means there's a copy of the page on disk in both the
        // db file and the journal. so safe to flush it. but need to make sure to remove it from 
        // dirty pages then. How do we inform the pager that they need to remove this page from
        // dirty pages?
        if (victim_page->is_dirty && victim_page->need_flushing) {
            // We need to do a cache spillover. instead of calling phase 1 commit from here
            // which is going to require the pCache to hold a pointer to its parent pager
            // we can just return a status code
            // rc: DirtyFlush
            result.status = PCacheResult::DirtyFlush;
            return result;
        }

        result.eviction.happened = true;
        result.eviction.page_num = victim_page->page_num;
        result.eviction.was_dirty = victim_page->is_dirty;

        // Cleanup: Free the resource taken up by the page
        PCacheResult remove_result = remove(victim_page->page_num);
        assert(remove_result == PCacheResult::Success);
    }

    // Saving the page in our cache.
    cache_map[page->page_num] = page;
    length++;
    if (page->refs_num == 0) {
        assert(!unpinned_pages->exists(page_num));
        unpinned_pages->add(page_num, page);
    }
    result.status = PCacheResult::Success;
    return result;
}

PCacheResult PCache::remove(int page_num) {
    // why would we even need to call this?
    // Yes we would need it since we would need to remove a page from cache if it's dirty and we want to rollback
    // Errors that could be encountered:
    // It's pinned down by some process that is reading it
    // RemovingPinnedPage
    auto it = cache_map.find(page_num);
    if (it == cache_map.end()) return PCacheResult::Success;

    Page *page = it->second;
    if (page->refs_num > 0) {
        return PCacheResult::RemovingPinnedPage;
    }

    assert(unpinned_pages->exists(page_num));
    unpinned_pages->remove(page_num);
    cache_map.erase(page_num);
    delete page;
    length--;
    return PCacheResult::Success;
}

void PCache::force_remove(int page_num) {
    auto it = cache_map.find(page_num);
    if (it == cache_map.end()) return;

    Page *page = it->second;
    if (page->refs_num == 0) {
        assert(unpinned_pages->exists(page_num));
        unpinned_pages->remove(page_num);
    } else {
        assert(!unpinned_pages->exists(page_num));
    }

    cache_map.erase(page_num);
    delete page;
    length--;
}

void PCache::pin_page(int page_num) {
    // Check if page exists in cache first
    auto it = cache_map.find(page_num);
    assert(it != cache_map.end());

    // Pager increments refs_num before calling this helper.
    Page *page = it->second;
    assert(page->refs_num > 0);

    if (page->refs_num == 1 && unpinned_pages->exists(page_num)) {
        // A 0 -> 1 transition means the page is no longer evictable.
        unpinned_pages->remove(page_num);
    } else {
        assert(page->refs_num == 1 || !unpinned_pages->exists(page_num));
    }
    // Don't increment refs_num. this will be done by the pager
}

void PCache::unpin_page(int page_num) {
    // check if page exists in the cache
    auto it = cache_map.find(page_num);
    assert(it != cache_map.end());

    // Pager decrements refs_num before calling this helper.
    Page *page = it->second;
    assert(page->refs_num == 0);

    // add to the unpinned_pages
    assert(!unpinned_pages->exists(page_num));
    unpinned_pages->add(page_num, page);
    // don't change refs_num. this will be done by the pager
}

int PCache::len() {
    return length;
}

int PCache::unpinned_len() {
    return unpinned_pages->len();
}
