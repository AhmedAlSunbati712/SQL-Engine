#pragma once
#include <unordered_map>
#include <DLList.h>

class PCache {
    public:
		PCache(): capacity(DEFAULT_CAPACITY) {};
		~PCache() {/* Should we free the pages if we are destructing the cache? */}
		Page *get(int page_num);
		Page *put(Page *page);
		void pin_page(int page_num); // If page is already pinned, do nothing. Else move out of unpinned pages
		void unpin_page(int page_num); // if page is already unpinned, do nothing. If not, move it into unpinned pages
		int len();
	private:
		static const int DEFAULT_CAPACITY = 64;
		int capacity = DEFAULT_CAPACITY;
		int length = 0;
		std::unordered_map<int, Page *> cache_map;
		DLList *unpinned_pages = new DLList();
};