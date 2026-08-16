# Pager + Cache

## Version 1

## Data Structures & Interfaces

### Page struct

- We know that we are not taking up more than 4KB when loading from disk. We can just then allocate a static array for the data so it lives in the same block of contiguous memory as the page, instead of storing a pointer to another region in the heap and having to deal with allocations and frees. Hence, `char data[4096]`
- Bookkeeping fields: `page_num`, `refs_num` the number of transactions pinning down this page (actively using it).
- `is_dirty.` We need this for a couple of reasons. First of all, for bookkeeping in the case that we call `get` to read a page and for some reason want to check if it’s dirty or not. The other reason is for journaling purposes. A page that has been marked dirty means that we already have a backup of it in the journal, so no need to back it up again. The third reason is that during LRU Page eviction, it might happen that the victim page to evict happens to be a dirty page. If that’s the case, we need to flush it to disk, and possibly flush the journal too. We need a way to find that out quickly, and having a boolean flag for that is the easiest way.
- `need_flushing.` This tells us whether a dirty page still needs to participate in the next journal spill / phase-one flush. A page can remain dirty after phase one, but stop needing another flush until it is written again.

```cpp
struct Page {
	char data[4096];
	int page_num;
	int refs_num;
	bool is_dirty;
	bool need_flushing;
}
```

### PCache

- A general hash map for fast O(1) lookups to find a page object by its number.
- Unpinned_pages will be implemented as a doubly-linked list. Least recently used unpinned page will be at the forefront. Most recently will be at the tail. Will implement O(1) lookups with a hashmap that maps page numbers to nodes.
- Two methods: get and put. That’s it

```cpp
class PCache {
	private:
		static const DEFAULT_CAPACITY = 64;
		int capacity = DEFAULT_CAPACITY;
		int length = 0;
		std::unordered_map<int, PageV2 *> cache_map;
		DLList *unpinned_pages = new DLList();
	public:
		PCache(): capacity(DEFAULT_CAPACITY) {};
		~PCache() {/* Should we free the pages if we are destructing the cache? */}
		PageV2 *get(int page_num);
		PageV2 *put(PageV2 *page);
		int len();
}
```

### Pager

```cpp
class Pager {
	public:
		Pager();
		PagerResult open(std::string db_file);
		PagerGetResult get(int page_num); // Returns a pinned PageV2*.
		PagerResult begin_write(int page_num);
		PagerResult commit_phase_one();
		PagerResult commit_phase_two();
		PagerResult rollback_transaction();
		PagerResult rollback_hot_journal();
	private:
		enum class WriteTxnState : std::uint8_t {
			None = 0,
			DirtyInMemory,
			JournalDurable,
		};

		std::unordered_map<int, DirtyPageEntry *> dirty_pages;

		std::fstream dbFile_handler;

		std::string db_name;
		std::string jFile_name;
		PCache *pCache;
		WriteTxnState write_txn_state;
}
```

### DLList

```cpp
class DLList {
	private:
		struct Node {
			Node() {};
			Node(PageV2 *page): page(page) {};
			PageV2 *page = nullptr;
			Node *next = nullptr;
			Node *prev = nullptr;
		};
		std::unordered_map<int, Node *> nodesMap;
		int length = 0;
		Node *head;
		Node *tail;
	public:
		DLList() {/*Fill in*/};
		~DLList();
		void add(int page_num, PageV2 *page);
		PageV2 *get(int page_num);
		PageV2 *remove(int page_num);
		bool exists(int page_num);
		int len();
}
```

## Invariants

- Only pages with `refs_num = 0` will be in unpinned_pages. Also, all pages with `refs_num = 0` will be in unpinned_pages.
- At any moment, `dirty_pages`  will hold pages that the client called write on. Doesn’t matter whether the client actually modified them or not. They will all have the `is_dirty` flag set too. Furthermore, any page that has `is_dirty` flag set will be in the `dirty_pages` .
- Before `Page::get` returns anything, we will ensure that the page is loaded in the cache.
- Before `Page::write` returns anything, we will ensure that the page is backed up in a journal file.
- `unpinned_pages` Will maintain a list of unpinned pages (under the hood, it’s a doubly-linked list with a hash map). The page at the very front of the list is the one that was least recently fully unpinned (i.e, `refs_num` = 0). Every time a page get fully unpinned, we add it to the tail of the list.
- No two `Page` objects will hold refs to the same page_num. NOT ALLOWED.
- Before phase one commit, every dirty page must have a corresponding rollback journal record in memory. After phase one commit, the journal is securely on disk.
- Before phase two commit, the journal is flushed to disk. After phase two commit, the dirty pages are flushed to disk and the journal is truncated. **Open question:** How do we ensure that the What
- `WriteTxnState::DirtyInMemory` means the current transaction can still be rolled back entirely from the pager's in-memory backup images.
- `WriteTxnState::JournalDurable` means this transaction has already crossed the durable-journal boundary, so abort must go through hot-journal recovery semantics rather than memory-only cleanup.

## State Transitions / Lifecycle

- `None -> DirtyInMemory` on the first successful `begin_write()` of a clean page.
- `DirtyInMemory -> JournalDurable` once phase one finishes making the rollback journal durable on disk.
- `JournalDurable` is sticky for the remainder of the transaction, even if the current dirty set later becomes empty because of cache spill.
- `rollback_transaction()` is the explicit local abort entrypoint:
  - if state is `DirtyInMemory`, it invalidates / restores the dirty cached pages from `backup_image`
  - if state is `JournalDurable`, it delegates to `rollback_hot_journal()`
- `rollback_hot_journal()` handles crash recovery and any abort path that already touched durable journal state on disk.

## Control Flow

### Reading a page when there’s enough capacity in the cache

```mermaid
graph TD
    Start([Client requests a page]) --> PagerGet[Pager calls get with the page number on the cache]
    PagerGet --> CacheHit{Cache hit?}

    %% Cache Hit Path (Left side)
    CacheHit -- Yes --> IsUnpinned{Is page in unpinned pages?}
    IsUnpinned -- Yes --> RemoveUnpinned[Remove from unpinned pages]
    RemoveUnpinned --> ReturnPager1[Return page to pager]
    IsUnpinned -- No --> ReturnPager1

    %% Cache Miss Path (Right side)
    CacheHit -- No --> ReadDisk[Read page from disk]
    ReadDisk --> IsNew{Is it a new page?}
    IsNew -- Yes --> InitBytes[Initialize it's bytes to zero]
    InitBytes --> BuildStruct[Build Page struct]
    IsNew -- No --> BuildStruct
    
    BuildStruct --> AddCache[Add to cache]
    AddCache --> ReturnPager2[Return page to pager]

    %% Final Steps
    ReturnPager1 --> Increment[Increment nrefs by 1]
    ReturnPager2 --> Increment[Increment nrefs by 1]
    Increment --> Final[/Return byte array of data/]
   
```

![Screenshot 2026-05-07 at 12.42.11 AM.png](attachment:47a677d3-d294-46b4-98b6-4d37c8fa199e:Screenshot_2026-05-07_at_12.42.11_AM.png)

### Handling full cache

```mermaid
graph TD
    Start[Add to cache] --> FreeSpot{Is there a free spot?}
    
    FreeSpot -- Yes --> AddHash[Add it to the hashtable]
    FreeSpot -- No --> Unpinned{Are there any unpinned pages?}
    
    Unpinned -- No --> ThrowError[THROW ERROR]
    Unpinned -- Yes --> Dirty{Is page dirty?}
    
    Dirty -- Yes --> JournalCheck{Is need_to_flush_journal true?}
    
    JournalCheck -- Yes --> FlushJournal[Flush journal]
    JournalCheck -- No --> FlushDisk[Flush page to disk]
    
    FlushJournal --> FlushDisk
    FlushDisk --> AddHash
    AddHash --> Return([Return])
```

![Screenshot 2026-05-07 at 12.41.35 AM.png](attachment:8db08a72-6896-48db-8323-c984a77b3d93:Screenshot_2026-05-07_at_12.41.35_AM.png)

### Writing to a page

```mermaid
graph TD
    Start([Client calls write]) --> DirtyCheck{is page <br/>already dirty?}
    
    DirtyCheck -- Yes --> Return([Return])
    
    DirtyCheck -- No --> CallRead[Call Pager::read]
    CallRead --> Increment[Increment refs_num by 1]
    Increment --> JournalCheck{Is there a <br/>journal file?}
    
    JournalCheck -- No --> CreateJournal[Create one, and save a <br/>handler to it]
    JournalCheck -- Yes --> AppendBackup[Append backup <br/>image of page to jFile]
    
    CreateJournal --> AppendBackup
    AppendBackup --> MarkDirty[Mark page dirty and <br/>add it to dirty list]
    
    MarkDirty --> Return
```

![Screenshot 2026-05-07 at 1.11.25 AM.png](attachment:bb5d3768-858e-4658-8d17-d1d323934331:Screenshot_2026-05-07_at_1.11.25_AM.png)

### Phase 1 Commit

```mermaid
graph TD
    Start([Client calls commit phase 1]) --> DirtyCheck{Are there dirty pages?}
    
    DirtyCheck -- No --> DoNothing([Do nothing])
    
    DirtyCheck -- Yes --> JournalCheck{Is there a journal file?}
    
    JournalCheck -- No --> Error[Throw error. Can't commit when there's no backup]
    
    JournalCheck -- Yes --> FlushJournal[Flush journal file to disk]
    
    FlushJournal --> FlushDB[Flush dirty pages to database]
    
    FlushDB --> Return([Return])
```

![Screenshot 2026-05-07 at 1.10.50 AM.png](attachment:4a9b1324-e3c3-4a18-bc90-a26d39bcef17:Screenshot_2026-05-07_at_1.10.50_AM.png)

### Phase 2 Commit

```mermaid
graph TD
    Start([Client calls commit phase 2]) --> Check{Is there a journal file and <br/>can we write to the <br/>handler?}
    
    Check -- No --> Error[Throw error]
    
    Check -- Yes --> Delete[Delete for version 1. <br/>Use better mechanisms for <br/>future versions]
    
    Delete --> Return([Return])
```

![Screenshot 2026-05-07 at 1.10.56 AM.png](attachment:dbf18a0c-fa9f-430e-afd3-d2f38e7555fc:Screenshot_2026-05-07_at_1.10.56_AM.png)

## Edge cases to deal with

- Writes after phase 1 commit and before phase 2 commit
    - Solution: Throw an error if the dirty list is not empty on entry to phase 2. Phase 1 should write all dirty pages to disk and remove the dirty pages list.

## Backlog

- In the flow for reading a page— If the page is not in cache and we need to read from disk, we probably need to check if the cache can accommodate this read even to begin with. If not, we probably should throw a runtime error
- In the future, we should expand the cache temporarily if there isn’t a victim to evict.
- Journal header. Two phase flush for journal (first one for the log records, second one for the header). No need to do same for dirty pages.
- Database header. Need to also incorporate file-change counts in later versions of commits
- We haven’t really talked about Pager::open yet! What should we do when a database file doesn’t exist as opposed to when it exists? Need to also add branches to each of the other methods if there’s no database opened. Also implement Pager::close
-
# Revision: June 24th, 2026: Page allocation for the pager
As I was writing the integration test for the pager, I realized one major issue with the current version (SHA `ea1be59`): The pager has no way of allocating new pages! It had the capability of reading and writing existing pages in a crash-safe manner. However, it couldn't handle cases where we needed a new page to write to. This also means that if there isn't an already existing database file, starting a new database is not possible. This is the cause of this revision. Simply adding a method called `allocate_page()` + `free_page(uint32_t page_num)` is not that easy. The reason for that is that adding or freeing pages is equivalent to changing the DB. Therefore, we have to handle some edge cases of rollbacks in case the DB crashes in the middle of a transaction that allocates or frees pages.

## Page allocation
In this design, we propose two mechanisms of allocating pages:

- Allocate from a previously freed page (Therefore we need to keep track of a list of free pages to reuse).
- Simply extending the DB with a new page if there's a free page to reusue.

The freepage list is going to be simply a doubly-linked list. Each node is simply a 4KB page, with the first 4 bytes storing the page number of the next free page in the freelist, the next 4 bytes storing the page number of the previous free page. Since we are using unsinged integers to store those, we are going to the the number of the pages in the database to represent the absence of a next/previous free page. For example, if the DB has 10 pages, then the last page on the freelist will have a next pointer holding the value 10.
```
[4 bytes | 4 bytes | EMPTY 4088 bytes]
[next_free_page_num | prev_free_page_num | Garbage OR Zeros]
```

The header page of the database will contain a pointer to the the first page of the freelist at the offset 24. Here's the structure of the database header:
```
[16 bytes | 4 bytes | 4 bytes | 4 bytes | 4 bytes | EMPTY 4064 bytes]
[magic_header_string | file_change_counter | db_page_count | freelist_head_page_num | freelist_page_count | Garbage OR Zeros]
```

Now, how do we handle requests for page allocation? Roughly, the approach is as the following: If the free_page count from the DB header is not zero, get a free page, save a backup of that page, it's previous page and its next page in the list if there exists one. Modify the previous page pointers, the next page pointers, and the header file accounting informatoin (number of free pages).

Otherwise, just return a zeroed-out array of 4KB with a page_num equal to the number of pages in the DB.
```
allocate_page()
	If free_page_count > 0:
		freelist_head_page_num <- header.freelist_head
		begin_write(freelist_head_page_num)
		freelist_head_page <- pCache.get(freelist_head_page_num)
		if (next_free_page_num <- freelist_head_page.next) != page_count: # only if freelist_head_page.next != page count
			begin_write(next_free_page_num)
			header.freelist_head <- next_free_page_num
			next_free_page <- pCache.get(freelist_next_free_page_num)
			next_free_page.prev <- 0
			header->freelist_head <-  next_free_page_num
		else:
			header->freelist_head <- page_count
		
		return {freelist_head_page_num, char[4kb]}
	else:
		# Don't extend the database file yet
		Create a page struct
		Add to cache
		mark as dirty
		add to dirty list
		return {db_page_count, char[4kb]}
```

For v1, freeing a page means we will just add it to the freelist. For now, let's traverse the freelist till the end and then add this one
```
free_page(page_num):
	begin_write(page_num)
	curr <- header
	while curr.next != page_count:
		seek to curr.next page offest
		read exact
		deserialize
	now add curr to our read cache
	add it to the dirty list
	mark it dirty
	set its next pointer to the page we want to free
	add the 

```

## Rolling back
This adds a little bit of changes to the journaling and rolling back after a crash. It matters in the following cases:
- **A new page was added to the database**: In that case, we need to truncate it out of the database. We can do this by first doing normal recovery then checking the page count from the header. Then ask ourselves what is the size of the database. Then calculate the page nums from that. If it ends up being more than the one in the header, we truncate.
- **DirtyPageEntry for new pages**: The backup image will be nullptr. Just to make sure we don't store an image for it in the journal.

I think that's it. One thing we will also need to do is the logic when opening a database file for the first time. If the file for the db doesn't exist, create one, create the header, write it to disk, flush and sync, then return to the user.
