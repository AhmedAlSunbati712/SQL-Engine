#pragma once
#include <unordered_map>
#include <DLList.h>
#include <Page.h>

enum class PCacheError : std::uint8_t {
	Success = 0,
	PageInCache, // PCache::put
	NoVictim, // PCache::put
	DirtyFlush, // PCache::put
	RemovingPinnedPage, // Possibly PCache::remove
	PinningNonexistantPage, // PCache::pin_page
	PinnedZeroRefsPage, // PCache::pin_page
	UnpinningNonexistantPage, // PCache::unpin_page
	UnpinnedNonZeroRefsPage, // PCache::unpin_page
};

class PCache {
    public:
		PCache();
        PCache(int capacity);
		~PCache();
		Page *get(int page_num);
		void put(Page *page);
        void remove(int page_num);
		void pin_page(int page_num); // If page is already pinned, do nothing. Else move out of unpinned pages
		void unpin_page(int page_num); // if page is already unpinned, do nothing. If not, move it into unpinned pages
		int len();
	private:
		static const int DEFAULT_CAPACITY = 64;
		int capacity = DEFAULT_CAPACITY;
		int length = 0; // Total number of pages in the cache. Pinned or unpinned
		std::unordered_map<int, Page *> cache_map;
		DLList *unpinned_pages = new DLList();
};