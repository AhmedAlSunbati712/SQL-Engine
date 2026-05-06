# Pager + Cacher
## Data Structures

### Page struct

- We know that we are not taking up more than 4KB when loading from disk. We can just then allocate a static array for the data so it lives in the same block of contiguous memory as the page, instead of storing a pointer to another region in the heap and having to deal with allocations and frees. Hence, `char data[4096]`
- Bookkeeping fields: `page_num`, `refs_num` the number of transactions pinning down this page (actively using it).
- `is_dirty.` We need this for a couple of reasons. First of all, for bookkeeping in the case that we call `get` to read a page and for some reason want to check if it’s dirty or not. The other reason is for journaling purposes. A page that has been marked dirty means that we already have a backup of it in the journal, so no need to back it up again. The third reason is that during LRU Page eviction, it might happen that the victim page to evict happens to be a dirty page. If that’s the case, we need to flush it to disk, and possibly flush the journal too. We need a way to find that out quickly, and having a boolean flag for that is the easiest way.

```cpp
struct Page {
	char data[4096];
	int page_num;
	int refs_num;
	bool is_dirty;
	bool need_to_flush_journal;
}
```

### PCache

- A general hash map for fast O(1) lookups to find a page object by its number.
- An unordered set of dirty pages for a couple of reasons. If the pager is trying to commit a transaction, we need to find all dirty pages and commit them. Walking all of the hash table to just find a couple of dirty pages is not efficient.
- A deque of unpinned pages that will be used for LRU eviction. The reason im using a deque here is because the least recently used unpinned page will always be at the front of the queue either way. So, I don’t need to do any of the double-linkedlist implementation and splicing heuristics to update the list of unpinned pages to reflect access patterns.
- Two methods: get and put. That’s it

```cpp
class PCache {
	private:
		static const DEFAULT_CAPACITY = 64;
	public:
		int capacity
		std::unordered_map<int, Page *> cache_map;
		std::unordered_set<Page *> dirty_pages;
		std::deque<Page *> unpinned_pages;
		PCache(): capacity(DEFAULT_CAPACITY) {};
		~PCache() {/* Should we free the pages if we are destructing the cache? */}
		Page *get(int page_num);
		Page *put(Page *page);
}
```

### Pager

```cpp
class Page {
	public:
		char[] *get(int page_num);
		bool write(char[] data);
		bool commit_phase_one();
		bool commit_phase_two();
	private:
		std::fstream dbFile_handler;
		std::fstream jFile_handler;

		std::string database_name;
		std::string jFile_name;
		PCache *pCache;
}
```