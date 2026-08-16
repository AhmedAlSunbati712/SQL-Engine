#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <DLList.h>
#include <PageV2.h>

enum class PCacheResult : std::uint8_t {
	Success = 0,
	NoVictim,
	DirtyFlush,
	WalPending,
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
		PageV2 *get(int page_num);
		PCachePutResult put(PageV2 *page);
        PCacheResult remove(int page_num);
        void force_remove(int page_num);
		void pin_page(int page_num); // Called after refs_num increments. On a 0 -> 1 transition, move out of unpinned pages
		void unpin_page(int page_num); // Called after refs_num decrements. On a 1 -> 0 transition, move into unpinned pages
		int len();
		int unpinned_len();
	private:
		static const int DEFAULT_CAPACITY = 64;
		int capacity = DEFAULT_CAPACITY;
		int length = 0; // Total number of pages in the cache. Pinned or unpinned
		mutable std::recursive_mutex mutex_;
		std::unordered_map<int, PageV2 *> cache_map;
		DLList *unpinned_pages = new DLList();
};
