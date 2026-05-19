#include <PCache.h>
#include <stdexcept>
#include <sstream>

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
        cache_map.erase(iterator->first);
        iterator = std::next(iterator);
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

Page *PCache::put(Page *page) {
    int page_num = page->page_num;

    // Check if the page already exists. If it does, throw a runtime_error
    // Should we just try to replace the page in the future instead?
    auto iterator = cache_map.find(page_num);
    if (iterator != cache_map.end()) {
        std::ostringstream oss;
        oss << "Error (PCache.put, PCache object: " << static_cast<void *>(this) << "): Page " << page_num << " already exists in cache.";
        throw std::runtime_error(oss.str());
    }
    
    // If The cache is full, find a victim unpinned page to evict and evict it
    if (length == capacity) {

        // No victim page to evict? throw an error. Later on we will expand the cache temporarily
        if (unpinned_pages->len() == 0) {
            std::ostringstream oss;
            oss << "Error (PCache.put, PCache Object: " << static_cast<void *>(this) << "): Failed to find a victim page to evict.";
            throw std::runtime_error(oss.str());
        }

        auto it = unpinned_pages->end(); // this returns an iterator that starts at tail
        it--; // Move to the most recently added node. This now is pointing to tail->prev

        // We need to stop when we pass the least recently added node
        auto it_head = unpinned_pages->begin(); // this returns an iterator that starts at head->next
        it_head--; // This is now an iterator that starts at head

        Page *victim_page = nullptr;
        while (it != it_head) {
            Page *page = *it;
            if (page->is_dirty == 0) {
                victim_page = page;
                break;
            }
            if (!victim_page) victim_page = page;
            it--;

        }

        // If a victim page page is dirty, we need to do some cleanup
        if (victim_page->is_dirty) {
            if (victim_page->need_to_flush_journal) {
                // TODO: we need to flush the journal to disk
            }
            // TODO: Flush the page to disk
        }

        // Cleanup: Free the resource taken up by the page
        unpinned_pages->remove(victim_page->page_num);
        cache_map.erase(victim_page->page_num);
        delete victim_page;
        length--;

    }

    // Saving the page in our cache.
    cache_map[page->page_num] = page;
    length++;
}

void PCache::remove(int page_num) {
    // why would we even need to call this?
    // Keep on hold
}

void PCache::pin_page(int page_num) {
    // Check if page exists in cache first
    auto it = cache_map.find(page_num);
    if (it == cache_map.end()) {
        std::ostringstream oss;
        oss << "Error (pin_page, PCache object: " << static_cast<void *>(this) << "): Page " << page_num << " doesn't exist in cache. Can't pin";
        throw std::runtime_error(oss.str());
    }

    // If page is not pinned, pin it
    Page *page = it->second;
    if (page->refs_num == 0 && unpinned_pages->exists(page_num)) {
        // Does the page have no refs and exists in unpinned pages?
        // and remove it from unpinned pages
        unpinned_pages->remove(page_num);
    } else if (page->refs_num == 0 && !unpinned_pages->exists(page_num)) {
        // This means the page has 0 refs but doesn't exist in unpinned pages
        // Something clearly has gone wrong
        std::ostringstream oss;
        oss << "Error (PCache::pin_page, PCache Object " << static_cast<void *>(this) << "): Page " << page_num << ". Page has 0 refs but doesn't exist in unpinned pages!";
        throw std::runtime_error(oss.str());
    }
    // Don't increment refs. this will be done by the pager
    return;
}

void PCache::unpin_page(int page_num) {
    // check if page exists in the cache
    auto it = cache_map.find(page_num);
    if (it == cache_map.end()) {
        std::ostringstream oss;
        oss << "Error (unpin_page, PCache object: " << static_cast<void *>(this) << "): Page " << page_num << " doesn't exist in cache. Can't unpin";
        throw std::runtime_error(oss.str());
    }

    // if page is pinned, unpin it
    Page *page = it->second;
    if (page->refs_num == 1 && !unpinned_pages->exists(page_num)) {
        // Page has one ref (that is being unpinned by this call)
        // The sceond condition is just a sanity check. if it has a non-zero refs_num,
        // it shouldn't be in the unpinned pages

        // add to the unpinned_pages
        unpinned_pages->add(page_num, page);
    } else if (page->refs_num != 0 && unpinned_pages->exists(page_num)) {
        // If the page has non-zero refs and exists in unpinned pages. something went wrong.
        std::ostringstream oss;
        oss << "Error (PCache::unpin, PCache Object " << static_cast<void *>(this) << "): Page " << page_num << ". Page has non-zero refs but also exists in unpinned pages!";
        throw std::runtime_error(oss.str());
    }
    // don't change refs_num. this will be done by the pager
    return;
}

int PCache::len() {
    return length;
}