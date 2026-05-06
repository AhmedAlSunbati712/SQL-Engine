# Pager + Cache
## Version 1

**Goal:** Build out a bare-bones MVP for the pager and page cache management. Should be able to read pages, pin pages, unpin pages, and do basic rollback journaling and apply appropriate cache coherence rules.

### Requirements

The MVP should:

- Be able to read 4KB pages from disk and pin them down if needed.
- Have LRU cache eviction on unpinned pages.
- Implement a basic version of writing to pages:
    - Before calling *_write(page number), ensure page is in cache first.
    - Open a journal file to save a backup of the page data.
    - Mark page dirty.
- Implement preliminary versions of Phase 1 & 2 commits:
    - Just flushing journal to disk
    - Flushing dirty pages to disk
    - Truncating the journal file
    - No modification to page 1 in the database yet.

### In-scope

- Data structures and classes for Cache, Page and Pager.
- Read and writes with basic versions of journaling.
- First-version of commits.

### Out-of-scope

- No locking and no synchronization.
- No integration with upstream modules (tree modules…etc).
- No hot-journal rollbacks