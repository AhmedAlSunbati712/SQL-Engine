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
    
    // Two cases
    // Case 1: The cache is full
    

}