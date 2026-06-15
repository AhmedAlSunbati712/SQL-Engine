#pragma once
#include <cstdint>
#include <unordered_map>
#include <DLList.h>
#include <Page.h>

enum class PCacheResult : std::uint8_t {
	Success = 0,
	NoVictim,
	DirtyFlush,
	RemovingPinnedPage
};

struct PCacheEviction {
	bool happened = false;
	int page_num = -1;
	bool was_dirty = false;
};

struct PCachePutResult {
	PCacheResult status = PCacheResult::Success;
	PCacheEviction eviction;
};

class PCache {
    public:
		PCache();
        PCache(int capacity);
		~PCache();
		Page *get(int page_num);
		PCachePutResult put(Page *page);
        PCacheResult remove(int page_num);
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
