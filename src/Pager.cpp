#include <Pager.h>
#include <DBHeaderCodec.h>
#include <DiskIO.h>
#include <JournalCodec.h>
#include <Log/Log.h>
#include <V2PageCodec.h>
#include <containers/BTreeOperation.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <span>
#include <unordered_set>

namespace {

constexpr std::uint32_t FREELIST_NULL_PAGE_NUM = 0;
constexpr std::size_t FREELIST_NEXT_OFFSET = 0;
constexpr std::size_t FREELIST_PREV_OFFSET = 4;

char *page_payload(PageV2 *page) {
    return page->data.data() + V2_PAGE_HEADER_SIZE;
}

void destroy_dirty_page_entry(DirtyPageEntry *entry) {
    delete[] entry->backup_image;
    delete entry;
}

PageV2 *make_page(int page_num, V2PageKind kind) {
    PageV2 *page = new PageV2();
    V2PageCodec::initialize(
        page->data,
        static_cast<std::uint32_t>(page_num),
        kind);
    page->page_num = static_cast<std::uint32_t>(page_num);
    page->refs_num = 0;
    page->is_dirty = false;
    page->need_flushing = false;
    return page;
}

bool close_fd_if_open(int &fd) {
    if (fd == -1) return true;
    try {
        disk::close_file(fd);
        fd = -1;
        return true;
    } catch (const std::exception &) {
        fd = -1;
        return false;
    }
}

PageEffect page_effect(PageEffectKind kind, const PageV2 *page) {
    PageEffect effect{
        .kind = kind,
        .page_num = page->page_num,
    };
    effect.after_image = page->data;
    return effect;
}

} // namespace

Pager::Pager() {
    pCache = new PCache();
    lock_manager = new LockMgr();
}

Pager::~Pager() {
    for (auto it = dirty_pages.begin(); it != dirty_pages.end(); ) {
        destroy_dirty_page_entry(it->second);
        it = dirty_pages.erase(it);
    }

    close_fd_if_open(db_fd);
    close_fd_if_open(journal_fd);
    delete pCache;
    delete lock_manager;
}

PagerResult Pager::open(std::string db_file) {
    assert(!is_open);
    if (is_open) return PagerResult::OpenDbFailed;

    db_name = std::move(db_file);
    jFile_name = db_name + "_journal";
    write_txn_state = WriteTxnState::None;
    txn_init_header_valid = false;

    // Check if the database exists on disk
    bool db_exists = false;
    try {
        db_exists = std::filesystem::exists(db_name);
    } catch (const std::exception &) {
        return PagerResult::OpenDbFailed;
    }

    if (!db_exists) {
        // If the database doesn't exist, create a file and flush
        // an initial header
        PagerResult create_result = create_new_database_file();
        if (create_result != PagerResult::Success) {
            db_name.clear();
            jFile_name.clear();
            return create_result;
        }
    } else {
        // If there does exist a file, first check if it's valie
        try {
            // A valid database file will have at least one page of size 4KB
            // and its size will always be a multipel of 4KB
            // If either of those are false, the DB is not valid
            std::uintmax_t file_size = std::filesystem::file_size(db_name);
            if (file_size < PAGE_SIZE || file_size % PAGE_SIZE != 0) {
                db_name.clear();
                jFile_name.clear();
                return PagerResult::DbHeaderCorrupt;
            }
        } catch (const std::exception &) {
            db_name.clear();
            jFile_name.clear();
            return PagerResult::OpenDbFailed;
        }
    }
    // Now, we are sure there's a db file on disk that is valid
    // open the database file
    try {
        db_fd = disk::open_file(db_name, O_RDWR);
    } catch (const std::exception &) {
        db_name.clear();
        jFile_name.clear();
        return PagerResult::OpenDbFailed;
    }

    // Set the is_open state
    is_open = true;

    // Make sure to rollback in case there's a hot journal on disk
    // We need to retry if there's a journal and we failed to grab the exclusive lock
    // By the next time we return success, the database would have recovered by another process
    PagerResult recovery_result;
    for (int i = 0; i < BUSY_RETRIES; i++) {
        recovery_result = maybe_recover_hot_journal();
        if (recovery_result != PagerResult::Busy) {
            // If the issue is not due to failure of grabbing an exclusive lock, break out
            break;
        }
    }
        
    if (recovery_result != PagerResult::Success) {
        // If recovery failed, close the db file handler and return the failure state
        // we cant continue.
        close_fd_if_open(db_fd);
        close_fd_if_open(journal_fd);
        db_name.clear();
        jFile_name.clear();
        is_open = false;
        write_txn_state = WriteTxnState::None;
        txn_init_header_valid = false;
        return recovery_result;
    }

    // Now, we need to load the header into our Pager state
    // so it can be referenced by the different operations (allocate_page, free_page for example)
    // TODO: Do we need to gain a shared lock for this? do we ref the the header and pin it down?
    PagerResult header_result = load_db_header_from_disk();
    if (header_result != PagerResult::Success) {
        // If we cant load the header, something went wrong.
        close_fd_if_open(db_fd);
        close_fd_if_open(journal_fd);
        db_name.clear();
        jFile_name.clear();
        is_open = false;
        write_txn_state = WriteTxnState::None;
        txn_init_header_valid = false;
        return header_result;
    }

    return PagerResult::Success;
}

PagerGetResult Pager::get(int page_num) {
    /**
     * Check in the cache. if it returns the page. return it
     * if it's not, read from disk, and put in the cache
     * increase refs
     * pin in the cache
     */
    PagerGetResult result;
    if (!is_open) {
        result.status = PagerResult::DatabaseNotOpen;
        return result;
    }

    // Make sure we are not accessing the header page or anything below it.
    // The upper boundary has to wait until we refresh the header under a stable read lock.
    assert(page_num > DB_HEADER_PAGE_NUM);
    if (page_num <= DB_HEADER_PAGE_NUM) {
        result.status = PagerResult::PageOutOfRange;
        return result;
    }
    Lock pre_lock = lock_manager->get_curr_lock();
    if (pre_lock == Lock::NOLOCK) {
        PagerResult entry_result = enter_from_nolock(Lock::SHARED);
        if (entry_result != PagerResult::Success) {
            result.status = entry_result;
            return result;
        }
    } else {
        // If we were already holding at least SHARED, no one could have committed underneath us.
        assert(pre_lock == Lock::SHARED || pre_lock == Lock::RESERVED || pre_lock == Lock::EXCLUSIVE);
    }

    // The header is now stable relative to our read privilege, so the upper boundary is meaningful.
    if (page_num >= static_cast<int>(db_header.db_page_count)) {
        if (pre_lock == Lock::NOLOCK) {
            PagerResult finish_result = finish_after_clean_state();
            if (finish_result != PagerResult::Success) std::abort();
        }
        result.status = PagerResult::PageOutOfRange;
        return result;
    }

    // Get or load the page. Basically asks the cache if it can return the page
    // if it's not in the cache, just load from disk and load in cache
    PageV2 *page = nullptr;
    PagerResult load_result = get_or_load_page(page_num, page);
    if (load_result != PagerResult::Success) {
        if (pre_lock == Lock::NOLOCK) {
            PagerResult finish_result = finish_after_clean_state();
            if (finish_result != PagerResult::Success) std::abort();
        }
        result.status = load_result;
        return result;
    }

    // Ref the page and pin it down
    page->refs_num += 1;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    result.page = page;
    return result;
}

PagerAllocateResult Pager::allocate_page(
    BTreeOperation& operation,
    V2PageKind kind
) {
    PagerAllocateResult result;
    if (!is_open) {
        result.status = PagerResult::DatabaseNotOpen;
        return result;
    }

    // Page zero owns the page count and freelist head. Keep its exclusive
    // latch in the B-tree operation until the complete WAL action is appended.
    operation.lock_exclusive(DB_HEADER_PAGE_NUM);

    PageV2 *freelist_head_page = nullptr;
    PageV2 *next_free_page = nullptr;
    std::uint32_t freelist_head_page_num = FREELIST_NULL_PAGE_NUM;
    std::uint32_t next_free_page_num = FREELIST_NULL_PAGE_NUM;

    if (db_header.freelist_page_count > 0) {
        freelist_head_page_num = db_header.freelist_head_page_num;
        operation.lock_exclusive(freelist_head_page_num);

        PagerResult head_result = get_or_load_page(
            static_cast<int>(freelist_head_page_num),
            freelist_head_page);
        if (head_result != PagerResult::Success) {
            result.status = head_result;
            return result;
        }

        std::uint32_t ignored_previous = FREELIST_NULL_PAGE_NUM;
        read_freelist_links(
            freelist_head_page,
            next_free_page_num,
            ignored_previous);

        if (next_free_page_num != FREELIST_NULL_PAGE_NUM) {
            operation.lock_exclusive(next_free_page_num);
            PagerResult next_result = get_or_load_page(
                static_cast<int>(next_free_page_num),
                next_free_page);
            if (next_result != PagerResult::Success) {
                result.status = next_result;
                return result;
            }
        }
    } else {
        // A brand-new page can be latched before its frame exists because the
        // latch manager is keyed by the stable logical page number.
        operation.lock_exclusive(db_header.db_page_count);
    }

    result = allocate_page(kind);
    if (result.status != PagerResult::Success) return result;

    PageV2 *header_page = pCache->get(DB_HEADER_PAGE_NUM);
    assert(header_page != nullptr);
    result.effects.push_back(page_effect(PageEffectKind::Write, header_page));

    if (next_free_page) {
        result.effects.push_back(page_effect(PageEffectKind::Write, next_free_page));
    }
    result.effects.push_back(page_effect(PageEffectKind::Allocate, result.page));
    return result;
}

PagerAllocateResult Pager::allocate_page(V2PageKind kind) {
    /**
     * Description: Used to allocate a new page for a write (or resuing a page from the freelist)
     */

    // Make sure the db file is open
    PagerAllocateResult result;
    if (!is_open) {
        result.status = PagerResult::DatabaseNotOpen;
        return result;
    }
    if (kind != V2PageKind::BTreeLeaf &&
        kind != V2PageKind::BTreeInternal) {
        result.status = PagerResult::InvalidPageKind;
        return result;
    }

    Lock pre_lock = lock_manager->get_curr_lock();
    if (pre_lock == Lock::NOLOCK) {
        PagerResult entry_result = enter_from_nolock(Lock::RESERVED);
        if (entry_result != PagerResult::Success) {
            result.status = entry_result;
            return result;
        }
    } else if (pre_lock == Lock::SHARED) {
        LockMgrStatus reserved_lock_acquire_result = lock_manager->lock(db_fd, Lock::RESERVED);
        if (reserved_lock_acquire_result == LockMgrStatus::Busy) {
            result.status = PagerResult::Busy;
            return result;
        }
    } else {
        assert(pre_lock == Lock::RESERVED || pre_lock == Lock::EXCLUSIVE);
    }

    // Check the freelist to avoid increasing the db size
    if (db_header.freelist_page_count > 0) {
        int freelist_head_page_num = static_cast<int>(db_header.freelist_head_page_num);
        assert(freelist_head_page_num > DB_HEADER_PAGE_NUM);
        assert(freelist_head_page_num < static_cast<int>(db_header.db_page_count));

        // reuse the head of the freelist
        PageV2 *freelist_head_page = nullptr;
        PagerResult load_head_result = get_or_load_page(freelist_head_page_num, freelist_head_page);
        if (load_head_result != PagerResult::Success) {
            result.status = load_head_result;
            restore_lock_after_failure(pre_lock);
            return result;
        }


        // Read the prev and the next pointers of the head
        std::uint32_t next_free_page_num = FREELIST_NULL_PAGE_NUM;
        std::uint32_t prev_free_page_num = FREELIST_NULL_PAGE_NUM;
        read_freelist_links(freelist_head_page, next_free_page_num, prev_free_page_num);
        assert(prev_free_page_num == FREELIST_NULL_PAGE_NUM); // The prev poitenr should point to the DB header

        // If there's a second free page, make sure to stitch back the linked list
        // We will load that page to journal it since we will be modifying it
        PageV2 *next_free_page = nullptr;
        if (next_free_page_num != FREELIST_NULL_PAGE_NUM) {
            assert(next_free_page_num < db_header.db_page_count);
            PagerResult load_next_result = get_or_load_page(static_cast<int>(next_free_page_num), next_free_page);
            if (load_next_result != PagerResult::Success) {
                result.status = load_next_result;
                restore_lock_after_failure(pre_lock);
                return result;
            }
        }

        // Mark the header as dirty
        PagerResult header_result = mark_header_dirty_for_mutation();
        if (header_result != PagerResult::Success) {
            result.status = header_result;
            restore_lock_after_failure(pre_lock);
            return result;
        }

        // Mark the head of the freelist is dirty
        PagerResult dirty_head_result = mark_loaded_page_dirty(freelist_head_page);
        if (dirty_head_result != PagerResult::Success) {
            result.status = dirty_head_result;
            restore_lock_after_failure(pre_lock);
            return result;
        }

        // If there's a next page, load it and make sure it's dirty too
        if (next_free_page) {
            PagerResult dirty_next_result = mark_loaded_page_dirty(next_free_page);
            if (dirty_next_result != PagerResult::Success) {
                result.status = dirty_next_result;
                restore_lock_after_failure(pre_lock);
                return result;
            }

            // Stitch the linked list pointers. This is the new head of the freelist
            std::uint32_t next_next_page_num = FREELIST_NULL_PAGE_NUM;
            std::uint32_t next_prev_page_num = FREELIST_NULL_PAGE_NUM;
            read_freelist_links(next_free_page, next_next_page_num, next_prev_page_num);
            write_freelist_links(next_free_page, next_next_page_num, FREELIST_NULL_PAGE_NUM);
        }

        // Modify the header accordingly
        db_header.freelist_head_page_num = next_free_page_num;
        db_header.freelist_page_count--;
        sync_db_header_to_cached_header_page(); // Sync the in-memory bytes

        // Reinitialize the reused frame with its new persistent page kind.
        V2PageCodec::initialize(
            freelist_head_page->data,
            static_cast<std::uint32_t>(freelist_head_page_num),
            kind);
        assert(freelist_head_page->refs_num == 0);
        freelist_head_page->refs_num = 1;
        pCache->pin_page(freelist_head_page_num);

        result.page_num = freelist_head_page_num;
        result.page = freelist_head_page;
        return result;
    }

    // If there's no free page on the freelist
    // let's get a brand new one appended to the end of file
    // make sure to mark the header of the database as dirty
    PagerResult header_result = mark_header_dirty_for_mutation();
    if (header_result != PagerResult::Success) {
        result.status = header_result;
        restore_lock_after_failure(pre_lock);
        return result;
    }

    // The new page number is the end of the database
    int new_page_num = static_cast<int>(db_header.db_page_count);
    db_header.db_page_count++; // Change the page count for the db
    sync_db_header_to_cached_header_page(); // sync the bytes

    // Make a brand new page for it
    PageV2 *new_page = make_page(new_page_num, kind);
    new_page->refs_num = 1;

    // Add it to cache
    PagerResult cache_result = cache_put(new_page);
    if (cache_result != PagerResult::Success) {
        db_header.db_page_count--;
        sync_db_header_to_cached_header_page();
        result.status = cache_result;
        restore_lock_after_failure(pre_lock);
        return result;
    }

    // Mark it as dirty and needing to be flushed
    new_page->is_dirty = true;
    new_page->need_flushing = true;

    // Add it to the dirty list but make sure the backup image entry is nullptr!
    auto it = dirty_pages.find(new_page_num);
    assert(it == dirty_pages.end());
    DirtyPageEntry *new_entry = new DirtyPageEntry();
    new_entry->page = new_page;
    dirty_pages[new_page_num] = new_entry;

    result.page_num = new_page_num;
    result.page = new_page;
    return result;
}

PagerMutationResult Pager::free_page(
    BTreeOperation& operation,
    std::uint32_t page_num
) {
    PagerMutationResult result;
    if (!is_open) {
        result.status = PagerResult::DatabaseNotOpen;
        return result;
    }

    // Freeing always changes page zero and the freed page. If a freelist
    // already exists, its old head also receives a new previous link.
    operation.lock_exclusive(DB_HEADER_PAGE_NUM);
    operation.lock_exclusive(page_num);

    const std::uint32_t old_head_page_num = db_header.freelist_head_page_num;
    if (old_head_page_num != FREELIST_NULL_PAGE_NUM) {
        operation.lock_exclusive(old_head_page_num);
    }

    result.status = free_page(static_cast<int>(page_num));
    if (result.status != PagerResult::Success) return result;

    PageV2 *header_page = pCache->get(DB_HEADER_PAGE_NUM);
    PageV2 *freed_page = pCache->get(static_cast<int>(page_num));
    assert(header_page != nullptr);
    assert(freed_page != nullptr);

    result.effects.push_back(page_effect(PageEffectKind::Write, header_page));
    if (old_head_page_num != FREELIST_NULL_PAGE_NUM) {
        PageV2 *old_head = pCache->get(static_cast<int>(old_head_page_num));
        assert(old_head != nullptr);
        result.effects.push_back(page_effect(PageEffectKind::Write, old_head));
    }
    result.effects.push_back(page_effect(PageEffectKind::Free, freed_page));
    return result;
}

PagerResult Pager::free_page(int page_num) {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    // Boundary check for anything below the first real payload page.
    // The upper boundary has to wait until the header is stable under lock.
    assert(page_num > DB_HEADER_PAGE_NUM);
    if (page_num <= DB_HEADER_PAGE_NUM) {
        return PagerResult::PageOutOfRange;
    }

    Lock pre_lock = lock_manager->get_curr_lock();
    if (pre_lock == Lock::NOLOCK) {
        PagerResult entry_result = enter_from_nolock(Lock::RESERVED);
        if (entry_result != PagerResult::Success) return entry_result;
    } else if (pre_lock == Lock::SHARED) {
        LockMgrStatus reserved_lock_acquire_result = lock_manager->lock(db_fd, Lock::RESERVED);
        if (reserved_lock_acquire_result == LockMgrStatus::Busy) return PagerResult::Busy;
    } else {
        assert(pre_lock == Lock::RESERVED || pre_lock == Lock::EXCLUSIVE);
    }

    if (page_num >= static_cast<int>(db_header.db_page_count)) {
        PagerResult restore_result = restore_lock_after_failure(pre_lock);
        if (restore_result != PagerResult::Success) std::abort();
        return PagerResult::PageOutOfRange;
    }

    // Get from cache or load from disk
    PageV2 *page_to_free = nullptr;
    PagerResult load_page_result = get_or_load_page(page_num, page_to_free);
    if (load_page_result != PagerResult::Success) {
        restore_lock_after_failure(pre_lock);
        return load_page_result;
    }

    // get the head of the freelist if it exists
    PageV2 *old_freelist_head = nullptr;
    if (db_header.freelist_head_page_num != FREELIST_NULL_PAGE_NUM) {
        assert(db_header.freelist_head_page_num != static_cast<std::uint32_t>(page_num));
        PagerResult load_head_result = get_or_load_page(static_cast<int>(db_header.freelist_head_page_num), old_freelist_head);
        if (load_head_result != PagerResult::Success) {
            restore_lock_after_failure(pre_lock);
            return load_head_result;
        }
    }

    // Mark the header as dirty
    PagerResult header_result = mark_header_dirty_for_mutation();
    if (header_result != PagerResult::Success){
        restore_lock_after_failure(pre_lock);
        return header_result;
    }

    // Mark the page to free as dirty since we are going to modify it
    PagerResult dirty_page_result = mark_loaded_page_dirty(page_to_free);
    if (dirty_page_result != PagerResult::Success) {
        restore_lock_after_failure(pre_lock);
        return dirty_page_result;
    }

    if (old_freelist_head) {
        // if there's an old page, stitch the links and pointers
        PagerResult dirty_head_result = mark_loaded_page_dirty(old_freelist_head);
        if (dirty_head_result != PagerResult::Success) {
            restore_lock_after_failure(pre_lock);
            return dirty_head_result;
        }

        std::uint32_t old_head_next_page_num = FREELIST_NULL_PAGE_NUM;
        std::uint32_t old_head_prev_page_num = FREELIST_NULL_PAGE_NUM;
        read_freelist_links(old_freelist_head, old_head_next_page_num, old_head_prev_page_num);
        write_freelist_links(old_freelist_head, old_head_next_page_num, static_cast<std::uint32_t>(page_num));
    }

    // After free_page succeeds this page is a freelist node, not a live user payload page any more.
    write_freelist_links(page_to_free, db_header.freelist_head_page_num, FREELIST_NULL_PAGE_NUM);
    db_header.freelist_head_page_num = static_cast<std::uint32_t>(page_num);
    db_header.freelist_page_count++;
    sync_db_header_to_cached_header_page();

    return PagerResult::Success;
}

PagerResult Pager::begin_write(int page_num) {
    /**
    * V1 for begin write: read page through get, ignore lock permissions, mark it as dirty
    * add to dirty pages disk and add to journal.
    */
    if (!is_open) return PagerResult::DatabaseNotOpen;

    assert(page_num > DB_HEADER_PAGE_NUM);
    if (page_num <= DB_HEADER_PAGE_NUM) {
        return PagerResult::PageOutOfRange;
    }

    Lock pre_lock = lock_manager->get_curr_lock();
    if (pre_lock == Lock::NOLOCK) {
        PagerResult entry_result = enter_from_nolock(Lock::RESERVED);
        if (entry_result != PagerResult::Success) return entry_result;
    } else if (pre_lock == Lock::SHARED) {
        LockMgrStatus reserved_byte_acquire_result = lock_manager->lock(db_fd, Lock::RESERVED);
        if (reserved_byte_acquire_result == LockMgrStatus::Busy) return PagerResult::Busy;
    } else {
        assert(pre_lock == Lock::RESERVED || pre_lock == Lock::EXCLUSIVE);
    }

    if (page_num >= static_cast<int>(db_header.db_page_count)) {
        PagerResult restore_result = restore_lock_after_failure(pre_lock);
        if (restore_result != PagerResult::Success) std::abort();
        return PagerResult::PageOutOfRange;
    }

    PageV2 *page = nullptr;
    PagerResult load_result = get_or_load_page(page_num, page);
    if (load_result != PagerResult::Success) {
        restore_lock_after_failure(pre_lock);
        return load_result;
    }

    PagerResult txn_result = ensure_transaction_started();
    if (txn_result != PagerResult::Success){
        restore_lock_after_failure(pre_lock);
        return txn_result;
    }

    return mark_loaded_page_dirty(page);
}

PagerResult Pager::ref_page(int page_num) {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    assert(page_num > DB_HEADER_PAGE_NUM);
    if (page_num <= DB_HEADER_PAGE_NUM) return PagerResult::PageOutOfRange;

    Lock pre_lock = lock_manager->get_curr_lock();
    if (pre_lock == Lock::NOLOCK) {
        PagerResult entry_result = enter_from_nolock(Lock::SHARED);
        if (entry_result != PagerResult::Success) return entry_result;
    } else {
        assert(pre_lock == Lock::SHARED || pre_lock == Lock::RESERVED || pre_lock == Lock::EXCLUSIVE);
    }

    PageV2 *page = pCache->get(page_num);
    if (!page) {
        // If reacquiring SHARED forced us to purge stale cache entries, this page may no longer be around.
        if (pre_lock == Lock::NOLOCK) {
            PagerResult finish_result = finish_after_clean_state();
            if (finish_result != PagerResult::Success) std::abort();
        }
        return PagerResult::PageNotCached;
    }

    page->refs_num++;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    return PagerResult::Success;
}

PagerResult Pager::unref_page(int page_num) {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    PageV2 *page = pCache->get(page_num);
    assert(page != nullptr);

    page->refs_num--;
    if (page->refs_num == 0) pCache->unpin_page(page_num);

    // Only release the final read privilege once we are fully back in a clean state.
    // If a write transaction is active, the current write-side lock has to stay alive.
    if (write_txn_state == WriteTxnState::None && pCache->unpinned_len() == pCache->len()) {
        PagerResult finish_result = finish_after_clean_state();
        if (finish_result != PagerResult::Success) std::abort();
    }
    return PagerResult::Success;
}

PagerGetRootResult Pager::get_btree_root() {
    PagerGetRootResult result{};
    if (!is_open) {
        result.status = PagerResult::DatabaseNotOpen;
        return result;
    }

    if (db_header.btree_root_page_num == 0) {
        result.status = PagerResult::EmptyBTree;
        result.page = nullptr;
        result.root_page_num = 0;
        return result;
    }

    PagerGetResult get_result = get(static_cast<int>(db_header.btree_root_page_num));
    result.status = get_result.status;
    result.page = get_result.page;
    result.root_page_num = db_header.btree_root_page_num;
    return result;
}

PagerMutationResult Pager::set_btree_root(
    BTreeOperation& operation,
    std::uint32_t root_page_num
) {
    PagerMutationResult result;
    if (!is_open) {
        result.status = PagerResult::DatabaseNotOpen;
        return result;
    }

    operation.lock_exclusive(DB_HEADER_PAGE_NUM);
    if (root_page_num != 0) operation.lock_exclusive(root_page_num);

    result.status = set_btree_root(root_page_num);
    if (result.status != PagerResult::Success) return result;

    PageV2 *header_page = pCache->get(DB_HEADER_PAGE_NUM);
    assert(header_page != nullptr);
    result.effects.push_back(page_effect(PageEffectKind::Write, header_page));
    return result;
}

PagerResult Pager::set_btree_root(std::uint32_t root_page_num) {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    PagerResult header_result = mark_header_dirty_for_mutation();
    if (header_result != PagerResult::Success) return header_result;

    db_header.btree_root_page_num = root_page_num;
    sync_db_header_to_cached_header_page();
    return PagerResult::Success;
}

PagerResult Pager::install_page_lsn(
    BTreeOperation& operation,
    std::uint32_t page_num,
    Lsn lsn
) {
    if (!is_open) return PagerResult::DatabaseNotOpen;
    if (lsn == 0) return PagerResult::PageCorrupt;

    // The page image cannot be published under an LSN unless this operation
    // still owns the exclusive logical latch that protected its modification.
    const std::optional<PageLatchMode> mode = operation.latch_mode(page_num);
    if (!mode || *mode != PageLatchMode::Exclusive) {
        return PagerResult::Busy;
    }

    PageV2 *page = nullptr;
    bool must_unref = false;
    if (page_num == DB_HEADER_PAGE_NUM) {
        PagerResult load_result = ensure_header_page_loaded(page);
        if (load_result != PagerResult::Success) return load_result;
    } else {
        PagerGetResult get_result = get(static_cast<int>(page_num));
        if (get_result.status != PagerResult::Success) return get_result.status;
        page = get_result.page;
        must_unref = true;
    }

    V2PageCodec::set_page_lsn(page->data, lsn);
    V2PageCodec::update_checksum(page->data);
    page->is_dirty = true;
    page->wal_pending = false;

    if (must_unref) {
        PagerResult unref_result = unref_page(static_cast<int>(page_num));
        if (unref_result != PagerResult::Success) return unref_result;
    }
    return PagerResult::Success;
}

PagerResult Pager::mark_wal_pending(
    BTreeOperation& operation,
    std::uint32_t page_num
) {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    // A frame may become non-evictable only while the operation still owns
    // the exclusive logical latch protecting its unlogged bytes.
    const std::optional<PageLatchMode> mode = operation.latch_mode(page_num);
    if (!mode || *mode != PageLatchMode::Exclusive) {
        return PagerResult::Busy;
    }

    PageV2 *page = nullptr;
    bool must_unref = false;
    if (page_num == DB_HEADER_PAGE_NUM) {
        PagerResult load_result = ensure_header_page_loaded(page);
        if (load_result != PagerResult::Success) return load_result;
    } else {
        PagerGetResult get_result = get(static_cast<int>(page_num));
        if (get_result.status != PagerResult::Success) return get_result.status;
        page = get_result.page;
        must_unref = true;
    }

    page->is_dirty = true;
    page->wal_pending = true;

    if (must_unref) {
        return unref_page(static_cast<int>(page_num));
    }
    return PagerResult::Success;
}


PagerResult Pager::commit_phase_one() {
    /**
     * Phase 1:
     * write the original images of the dirty pages to the journal
     * flush the journal records first
     * then go back and update the section header with the page count and flush again
     * once the journal is durable, write the modified pages to the db file and flush those too
     */
    if (!is_open) return PagerResult::DatabaseNotOpen;
    assert(lock_manager->get_curr_lock() == Lock::RESERVED || lock_manager->get_curr_lock() == Lock::EXCLUSIVE);

    // If there are no pages that still need flushing, there is nothing to commit here
    bool has_pages_to_flush = false;
    for (const auto &[page_num, dPage_entry] : dirty_pages) {
        (void)page_num;
        if (dPage_entry->page && dPage_entry->page->need_flushing) {
            has_pages_to_flush = true;
            break;
        }
    }
    if (!has_pages_to_flush) {
        return PagerResult::Success;
    }

    // commit_phase_one is the point where we have to stop being just an in-memory writer.
    // If we dont own EXCLUSIVE before touching the journal, we are not allowed to proceed.
    Lock pre_lock = lock_manager->get_curr_lock();
    bool had_durable_journal_already = (write_txn_state == WriteTxnState::JournalDurable);
    if (pre_lock != Lock::EXCLUSIVE) {
        LockMgrStatus exclusive_lock_result = lock_manager->lock(db_fd, Lock::EXCLUSIVE);
        if (exclusive_lock_result == LockMgrStatus::Busy) return PagerResult::Busy;
    }

    auto fail_before_durable_boundary = [&](PagerResult failure_result) -> PagerResult {
        if (!had_durable_journal_already && write_txn_state != WriteTxnState::JournalDurable) {
            // If we havent crossed the durable boundary yet, we can back out to the earlier write state.
            if (lock_manager->unlock(db_fd, pre_lock) != LockMgrStatus::Success) std::abort();
        }
        return failure_result;
    };

    // Check if we already started a journal for this transaction before.
    // If yes, then this is a spillover phase 1 and we append a new section.
    bool journal_exists = false;
    PagerResult journal_check = journal_has_contents(journal_exists);
    if (journal_check != PagerResult::Success) return fail_before_durable_boundary(journal_check);
    JournalHeader jHeader;

    if (!journal_exists) {
        // First section for this transaction. create a brand new journal file.
        try {
            if (journal_fd == -1) {
                journal_fd = disk::open_file(jFile_name, O_RDWR | O_CREAT | O_TRUNC, 0644);
            } else {
                disk::truncate_file(journal_fd, 0);
            }
        } catch (const std::exception &) {
            return fail_before_durable_boundary(PagerResult::JournalCreateFailed);
        }

    } else {
        // Journal already exists from an earlier spill.
        // Open it and verify that the first header is sane before we append another section.
        if (journal_fd == -1) {
            try {
                journal_fd = disk::open_file(jFile_name, O_RDWR);
            } catch (const std::exception &) {
                return fail_before_durable_boundary(PagerResult::JournalOpenFailed);
            }
        }

        char header_bytes[JOURNAL_HEADER_SIZE];
        try {
            std::span<char> header_bytes_span(header_bytes, JOURNAL_HEADER_SIZE);
            disk::read_exact_at(journal_fd, header_bytes_span, 0);
        } catch (const std::exception &) {
            return fail_before_durable_boundary(PagerResult::JournalOpenFailed);
        }

        Journal::deserialize_jHeader(jHeader, header_bytes);

        bool valid_journal = Journal::validate_journal_header(jHeader);
        if (!valid_journal) {
            return fail_before_durable_boundary(PagerResult::JournalCorrupt);
        }
    }

    // The new section header must start on a page boundary
    std::streamoff curr_offset = 0;
    try {
        curr_offset = static_cast<std::streamoff>(disk::file_size(journal_fd));
    } catch (const std::exception &) {
        return fail_before_durable_boundary(PagerResult::JournalHeaderWriteFailed);
    }
    curr_offset = align_to_page_boundary(curr_offset);

    // The journal section needs to know the db page count from before this transaction started.
    // This is what rollback will use if it has to shrink the db back down.
    assert(txn_init_header_valid);
    jHeader.nonce = Journal::generate_nonce();
    jHeader.init_db_page_count = txn_init_header.db_page_count;
    jHeader.page_count = 0;

    // Write a provisional header first with page_count = 0.
    // We come back later and rewrite it once we know how many records we actually appended.
    char jHeader_bytes[JOURNAL_HEADER_SIZE];
    try {
        Journal::serialize_jHeader(jHeader, jHeader_bytes);
        std::span<const char> jHeader_bytes_span(jHeader_bytes, JOURNAL_HEADER_SIZE);
        disk::write_exact_at(journal_fd, jHeader_bytes_span, curr_offset);
    } catch (const std::exception &) {
        return fail_before_durable_boundary(PagerResult::JournalHeaderWriteFailed);
    }

    std::streamoff journal_write_offset = curr_offset + static_cast<std::streamoff>(PAGE_SIZE);

    // Append the backup image of every page that still needs flushing.
    // Appended pages have backup_image == nullptr since there is no older image to restore,
    // so they do not get a journal record.
    for (const auto& [page_num, dirty_page] : dirty_pages) {
        if (!dirty_page->page->need_flushing || dirty_page->backup_image == nullptr) {
            continue;
        }

        JournalPageRecord jPage_record;
        jPage_record.page_num = page_num;
        for (int i = 0; i < PAGE_SIZE; i++) {
            jPage_record.data[i] = dirty_page->backup_image[i];
        }
        jPage_record.checksum = Journal::checksum(jHeader.nonce, jPage_record.data);
        jHeader.page_count++;

        char jPage_record_bytes[JOURNAL_PAGE_RECORD];
        Journal::serialize_jPage_record(jPage_record, jPage_record_bytes);

        try {
            std::span<const char> jPage_record_bytes_span(jPage_record_bytes, JOURNAL_PAGE_RECORD);
            disk::write_exact_at(journal_fd, jPage_record_bytes_span, journal_write_offset);
        } catch (const std::exception &) {
            return fail_before_durable_boundary(PagerResult::JournalRecordWriteFailed);
        }
        journal_write_offset += static_cast<std::streamoff>(JOURNAL_PAGE_RECORD);
    }

    // Flush and sync the journal records first.
    // At this point the header still says page_count = 0, so crash recovery should ignore this section.
    try {
        disk::sync_file_to_disk_fd(journal_fd);
    } catch (const std::exception &) {
        return fail_before_durable_boundary(PagerResult::JournalFlushFailed);
    }

    // Now rewrite the header with the real page_count and flush again.
    // Once this is on disk, the whole journal section becomes valid and replayable.
    char jHeader_bytes_final[JOURNAL_HEADER_SIZE];
    try {
        Journal::serialize_jHeader(jHeader, jHeader_bytes_final);
        std::span<const char> jHeader_bytes_final_span(jHeader_bytes_final, JOURNAL_HEADER_SIZE);
        disk::write_exact_at(journal_fd, jHeader_bytes_final_span, curr_offset);
    } catch (const std::exception &) {
        return fail_before_durable_boundary(PagerResult::JournalHeaderWriteFailed);
    }

    try {
        disk::sync_file_to_disk_fd(journal_fd);
    } catch (const std::exception &) {
        return fail_before_durable_boundary(PagerResult::JournalFlushFailed);
    }
    write_txn_state = WriteTxnState::JournalDurable;

    // The backup images are durable now, so it is finally safe to overwrite the db pages.
    for (const auto &[page_num, dPage_entry] : dirty_pages) {
        if (dPage_entry->page->wal_pending) return PagerResult::CacheSpillFailed;

        // WAL must reach stable storage before a page carrying that record's
        // LSN is allowed to overwrite the database file.
        const Lsn page_lsn = V2PageCodec::page_lsn(dPage_entry->page->data);
        if (page_lsn != 0) {
            if (!log_) return PagerResult::CacheSpillFailed;
            try {
                log_->sync_through(page_lsn);
            } catch (...) {
                return PagerResult::CacheSpillFailed;
            }
        }

        // The common checksum covers the complete persistent page and must
        // reflect the final bytes that are about to reach the database file.
        V2PageCodec::update_checksum(dPage_entry->page->data);
        std::span<const char> new_page_data(dPage_entry->page->data);
        try {
            disk::write_exact_at(
                db_fd,
                new_page_data,
                static_cast<std::streamoff>(page_num) * static_cast<std::streamoff>(PAGE_SIZE)
            );
        } catch (const std::exception &) {
            return PagerResult::DbWriteFailed;
        }
    }

    try {
        disk::sync_file_to_disk_fd(db_fd);
    } catch (const std::exception &) {
        return PagerResult::DbFlushFailed;
    }

    // The pages are still dirty until phase 2 truncates the journal.
    // But they no longer need flushing because their latest version is now on disk.
    for (auto &[page_num, dPage_entry] : dirty_pages) {
        (void)page_num;
        dPage_entry->page->is_dirty = true;
        dPage_entry->page->need_flushing = false;
    }
    return PagerResult::Success;
}

PagerResult Pager::commit_phase_two() {
    if (!is_open) return PagerResult::DatabaseNotOpen;
    assert(write_txn_state == WriteTxnState::JournalDurable);
    assert(lock_manager->get_curr_lock() == Lock::EXCLUSIVE);

    // Phase 2 only makes sense if there is a journal on disk that we can invalidate
    bool valid_journal = false;
    PagerResult journal_check = journal_has_contents(valid_journal);
    if (journal_check != PagerResult::Success) return PagerResult::JournalTruncateFailed;
    assert(valid_journal);
    if (!valid_journal) return PagerResult::JournalTruncateFailed;

    // Truncate the journal to zero bytes.
    // Once this succeeds, the transaction is fully committed.
    if (journal_fd == -1) {
        try {
            journal_fd = disk::open_file(jFile_name, O_RDWR);
        } catch (const std::exception &) {
            return PagerResult::JournalTruncateFailed;
        }
    }
    try {
        disk::truncate_file(journal_fd, 0);
    } catch (const std::exception &) {
        return PagerResult::JournalTruncateFailed;
    }

    // The transaction is over now, so clear the dirty bookkeeping for all of its pages
    for (auto it = dirty_pages.begin(); it != dirty_pages.end(); ) {
        DirtyPageEntry *dPage_entry = it->second;
        if (dPage_entry->page) {
            dPage_entry->page->is_dirty = false;
            dPage_entry->page->need_flushing = false;
        }
        destroy_dirty_page_entry(dPage_entry);
        it = dirty_pages.erase(it);
    }
    write_txn_state = WriteTxnState::None;
    txn_init_header_valid = false;
    return finish_after_clean_state();
}

PagerResult Pager::rollback_transaction() {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    // No active write transaction. nothing to rollback.
    if (write_txn_state == WriteTxnState::None) return PagerResult::Success;

    // The transaction never made it to a durable journal.
    // Just restore the in-memory cache state.
    if (write_txn_state == WriteTxnState::DirtyInMemory) {
        PagerResult cleanup_result = cleanup_transaction_cache();
        if (cleanup_result != PagerResult::Success) return cleanup_result;
        return finish_after_clean_state();
    }

    // A durable journal exists, so rollback has to replay it.
    assert(write_txn_state == WriteTxnState::JournalDurable);
    assert(lock_manager->get_curr_lock() == Lock::EXCLUSIVE);

    PagerResult replay_result = replay_hot_journal_under_exclusive();
    if (replay_result != PagerResult::Success) return replay_result;

    PagerResult header_result = load_db_header_from_disk();
    if (header_result != PagerResult::Success) return header_result;
    return finish_after_clean_state();
}

PagerResult Pager::rollback_hot_journal() {
    /**
     * TODO: since we have multiple headers now, what we probably need to do is the following:
     * while the current the current offset is not at the end of the journal file, read a number of bytes
     * from disk equal to the size of a header. If it's not a valid header, stop here and finalize recovery
     * if it's a valid header, get the number of pages that are in this sub-journal records and for i <- 1 to record_num (or wtvr):
     *      read a page deserialize and validate and do all the stuff we are doing and rollback
     * make sure we are at a page boundary. seek to the next smallest page boundary (or stay at the current one if its a page boundary)
     * Then eventually go over each page in dirty pages and clean up by removing it from the cache. does that sound like a good plan?
     */
    if (!is_open) return PagerResult::DatabaseNotOpen;
    Lock curr_lock = lock_manager->get_curr_lock();
    if (curr_lock == Lock::NOLOCK) {
        LockMgrStatus exclusive_lock_status = lock_manager->lock(db_fd, Lock::EXCLUSIVE);
        if (exclusive_lock_status != LockMgrStatus::Success) return PagerResult::Busy;

        PagerResult replay_result = replay_hot_journal_under_exclusive();
        if (replay_result != PagerResult::Success) return replay_result;

        if (lock_manager->unlock(db_fd, Lock::NOLOCK) != LockMgrStatus::Success) std::abort();
        return PagerResult::Success;
    }

    assert(curr_lock == Lock::EXCLUSIVE);
    PagerResult replay_result = replay_hot_journal_under_exclusive();
    if (replay_result != PagerResult::Success) return replay_result;

    PagerResult header_result = load_db_header_from_disk();
    if (header_result != PagerResult::Success) return header_result;
    return finish_after_clean_state();
}

/** Private helpers */

PagerResult Pager::journal_has_contents(bool &has_contents) {
    // A journal has contents if it exists and its fd reports a non-zero size.
    has_contents = false;

    if (journal_fd != -1) {
        try {
            has_contents = disk::file_size(journal_fd) > 0;
        } catch (const std::exception &) {
            return PagerResult::JournalOpenFailed;
        }
        return PagerResult::Success;
    }

    try {
        has_contents = std::filesystem::exists(jFile_name);
    } catch (const std::exception &) {
        return PagerResult::JournalOpenFailed;
    }

    if (!has_contents) return PagerResult::Success;

    try {
        journal_fd = disk::open_file(jFile_name, O_RDWR);
        has_contents = disk::file_size(journal_fd) > 0;
    } catch (const std::exception &) {
        close_fd_if_open(journal_fd);
        return PagerResult::JournalOpenFailed;
    }

    return PagerResult::Success;
}

PagerResult Pager::maybe_recover_hot_journal() {
    // This is mainly used in the following places:
    // - Opening a db: If we try to open a db, and then find that there's a hot journal, we need to rollback
    // - Get: A process calling get could either be the process doing the write transaction or another process 
    //        that is reading after a writer process crashed. In the first case, we don't want to rollback. that's
    //        what the if statement is doing. In the latter, we do want to rollback
    // - begin_write: similar reasoning
    if (write_txn_state != WriteTxnState::None) return PagerResult::Success;
    if (lock_manager->get_curr_lock() != Lock::NOLOCK) return PagerResult::Success;

    bool hot_journal_exists = false;
    PagerResult journal_check = journal_has_contents(hot_journal_exists);
    if (journal_check != PagerResult::Success) return journal_check;
    if (!hot_journal_exists) return PagerResult::Success;
    return rollback_hot_journal();
}

PagerResult Pager::enter_from_nolock(Lock target_lock) {
    // This is the common entry point whenever the pager is completely idle and needs
    // to become an active reader or writer again.
    assert(lock_manager->get_curr_lock() == Lock::NOLOCK);
    assert(write_txn_state == WriteTxnState::None);
    assert(target_lock == Lock::SHARED || target_lock == Lock::RESERVED);

    // First recover if another process crashed and left a hot journal behind.
    PagerResult recovery_result;
    for (int i = 0; i < BUSY_RETRIES; i++) {
        recovery_result = maybe_recover_hot_journal();
        if (recovery_result != PagerResult::Busy) break;
    }
    if (recovery_result != PagerResult::Success) return recovery_result;

    // Then acquire the stable privilege we actually need for the caller.
    LockMgrStatus lock_result = lock_manager->lock(db_fd, target_lock);
    if (lock_result == LockMgrStatus::Busy) return PagerResult::Busy;

    // Only after the lock is stable is it safe to trust the header we reload here.
    PagerResult header_result = load_db_header_from_disk();
    if (header_result != PagerResult::Success) {
        if (lock_manager->unlock(db_fd, Lock::NOLOCK) != LockMgrStatus::Success) std::abort();
        return header_result;
    }
    return PagerResult::Success;
}

PagerResult Pager::finish_after_clean_state() {
    assert(write_txn_state == WriteTxnState::None);
    assert(dirty_pages.empty());

    bool has_page_refs = (pCache->len() != pCache->unpinned_len());
    Lock curr_lock = lock_manager->get_curr_lock();

    if (has_page_refs) {
        // If any refs are still alive, we must hold at least SHARED when we leave this helper.
        assert(curr_lock != Lock::NOLOCK);
        if (lock_manager->unlock(db_fd, Lock::SHARED) != LockMgrStatus::Success) std::abort();
        return PagerResult::Success;
    }

    if (lock_manager->unlock(db_fd, Lock::NOLOCK) != LockMgrStatus::Success) std::abort();
    return PagerResult::Success;
}

PagerResult Pager::restore_lock_after_failure(Lock pre_lock) {
    if (write_txn_state == WriteTxnState::JournalDurable) {
        // If the journal is durable already, we should still be sitting in EXCLUSIVE here.
        // Trying to reacquire it would just hide a bug in the lock flow.
        assert(lock_manager->get_curr_lock() == Lock::EXCLUSIVE);
        return PagerResult::Success;
    }

    if (write_txn_state == WriteTxnState::DirtyInMemory) {
        // Every current caller reaches here only after it already acquired RESERVED.
        // So if we somehow lost it, that is a pager bug and not something to silently repair.
        Lock curr_lock = lock_manager->get_curr_lock();
        assert(curr_lock == Lock::RESERVED || curr_lock == Lock::EXCLUSIVE);
        return PagerResult::Success;
    }

    assert(write_txn_state == WriteTxnState::None);
    assert(dirty_pages.empty());

    if (pre_lock == Lock::NOLOCK) {
        return finish_after_clean_state();
    }

    if (lock_manager->unlock(db_fd, pre_lock) != LockMgrStatus::Success) std::abort();
    return PagerResult::Success;
}

PagerResult Pager::replay_hot_journal_under_exclusive() {
    // By the time we enter here, the caller already decided this recovery is real and
    // made sure we are the only process allowed to touch the DB image.
    assert(lock_manager->get_curr_lock() == Lock::EXCLUSIVE);

    // Check if a journal has already been started for this transaction
    bool valid_journal = false;
    PagerResult journal_check = journal_has_contents(valid_journal);
    if (journal_check != PagerResult::Success) return PagerResult::JournalOpenFailed;

    // Journal file is empty or missing. we cant rollback
    if (!valid_journal) {
        if (write_txn_state == WriteTxnState::DirtyInMemory) {
            return cleanup_transaction_cache();
        }
        assert(write_txn_state != WriteTxnState::JournalDurable);
        return PagerResult::JournalOpenFailed;
    }

    // Open the journal file
    if (journal_fd == -1) {
        try {
            journal_fd = disk::open_file(jFile_name, O_RDWR);
        } catch (const std::exception &) {
            return PagerResult::JournalOpenFailed;
        }
    }
    std::size_t journal_file_size = 0;
    try {
        journal_file_size = disk::file_size(journal_fd);
    } catch (const std::exception &) {
        return PagerResult::JournalOpenFailed;
    }
    // Store the number of pages we restored. This is used in case multiple backup images for the same page
    // were written to the journal. We want to keep the first one only
    std::unordered_set<int> restored_page_nums;
    std::streamoff curr_offset = 0;
    bool stop_recovery = false;
    bool has_recovery_target_page_count = false; // Have we already seen at least one valid journal header from which we learned the database size to restore to?
    std::uint32_t recovery_target_page_count = 0;

    // Keep looping until we hit end of file or we abruptly stop the recovery process
    while (curr_offset < static_cast<std::streamoff>(journal_file_size) && !stop_recovery) {
        // We are at the start of a new section probably
        // so we need to read the journal header of this section
        char jHeader_bytes[JOURNAL_HEADER_SIZE];
        try {
            std::span<char> jHeader_bytes_span(jHeader_bytes, JOURNAL_HEADER_SIZE);
            disk::read_exact_at(journal_fd, jHeader_bytes_span, curr_offset);
        } catch (const std::exception &) {
            if (curr_offset == 0) return PagerResult::JournalOpenFailed;
            break;
        }

        // Deserialize the journal header bytes
        JournalHeader jHeader;
        Journal::deserialize_jHeader(jHeader, jHeader_bytes);

        // Validate the journal header
        bool is_valid_header = Journal::validate_journal_header(jHeader);
        if (!is_valid_header) {
            if (curr_offset == 0) {
                // If this is the first header of the journal, it means the whole journal is corrupted
                // finalize it here
                try {
                    disk::truncate_file(journal_fd, 0);
                } catch (const std::exception &) {
                    return PagerResult::JournalTruncateFailed;
                }
                return PagerResult::JournalCorrupt;
            }
            break;
        }

        if (!has_recovery_target_page_count) {
            // The initial page count of the db before the transaction
            // We load it from the first header in the journal
            // then set has_recovery_target_page_count to true so we never reload
            recovery_target_page_count = jHeader.init_db_page_count;
            has_recovery_target_page_count = true;
        } else if (recovery_target_page_count != jHeader.init_db_page_count) {
            // All the headers should have the same initial db page count
            return PagerResult::JournalCorrupt;
        }

        // Get the number of pages for this section
        int page_count = static_cast<int>(jHeader.page_count);
        std::streamoff records_offset = curr_offset + static_cast<std::streamoff>(PAGE_SIZE);

        // Read each record, and write it to the db file
        for (int i = 0; i < page_count; i++) {
            char jPage_record_bytes[JOURNAL_PAGE_RECORD];
            std::streamoff j_offset = records_offset + static_cast<std::streamoff>(i) * static_cast<std::streamoff>(JOURNAL_PAGE_RECORD);
            try {
                std::span<char> jPage_record_bytes_span(jPage_record_bytes, JOURNAL_PAGE_RECORD);
                disk::read_exact_at(journal_fd, jPage_record_bytes_span, j_offset);
            } catch (const std::exception &) {
                stop_recovery = true;
                break;
            }

            JournalPageRecord jPage_record;
            Journal::deserialize_jPage_record(jPage_record, jPage_record_bytes);

            bool is_valid_record = Journal::validate_journal_record_checksum(jPage_record, jHeader);
            if (!is_valid_record) {
                // Stop here man. It's not valid so anything afterwards is not valid
                stop_recovery = true;
                break;
            }

            int db_page_num = static_cast<int>(jPage_record.page_num);
            if (restored_page_nums.find(db_page_num) != restored_page_nums.end()) {
                continue;
            }
            restored_page_nums.insert(db_page_num);

            std::streamoff db_offset = static_cast<std::streamoff>(db_page_num) * static_cast<std::streamoff>(PAGE_SIZE);
            try {
                std::span<const char> jPage_record_data_span(jPage_record.data, PAGE_SIZE);
                disk::write_exact_at(db_fd, jPage_record_data_span, db_offset);
            } catch (const std::exception &) {
                return PagerResult::DbWriteFailed;
            }
        }

        if (stop_recovery) break;

        // A journal header will always be on a a page boundary
        std::streamoff next_header_offset = align_to_page_boundary(
            records_offset + static_cast<std::streamoff>(page_count) * static_cast<std::streamoff>(JOURNAL_PAGE_RECORD)
        );
        if (next_header_offset >= static_cast<std::streamoff>(journal_file_size)) {
            break;
        }
        curr_offset = next_header_offset;
    }

    try {
        disk::sync_file_to_disk_fd(db_fd);
    } catch (const std::exception &) {
        return PagerResult::DbFlushFailed;
    }

    // Make sure to truncate the db file if the transaction changed it
    if (has_recovery_target_page_count) {
        try {
            std::size_t current_db_size = disk::file_size(db_fd);
            std::streamoff target_db_size = static_cast<std::streamoff>(recovery_target_page_count) * static_cast<std::streamoff>(PAGE_SIZE);
            if (current_db_size > target_db_size) {
                disk::truncate_file(db_fd, target_db_size);
            }
        } catch (const std::exception &) {
            return PagerResult::DbFlushFailed;
        }
    }

    // Truncate the journal to zero bytes to invalidate it after recovery.
    try {
        disk::truncate_file(journal_fd, 0);
    } catch (const std::exception &) {
        return PagerResult::JournalTruncateFailed;
    }

    // If the process the aborted the transaction is the one rolling back, clear the transaction cache
    if (write_txn_state != WriteTxnState::None || !dirty_pages.empty()) {
        PagerResult cleanup_result = cleanup_transaction_cache();
        if (cleanup_result != PagerResult::Success) return cleanup_result;
    }

    return PagerResult::Success;
}

PagerResult Pager::cleanup_transaction_cache() {
    // This invalidates the current dirty pages in cache for recovery from a failed write transaction
    // called in rollback_hot_journal only iff the process calling rollback is the one that was doing the transaction
    // or if the dirty pages list is not empty

    for (auto it = dirty_pages.begin(); it != dirty_pages.end(); ) {
        int page_num = it->first;
        DirtyPageEntry *dPage_entry = it->second;
        PageV2 *cached_page = pCache->get(page_num);

        if (!cached_page) {
            destroy_dirty_page_entry(dPage_entry);
            it = dirty_pages.erase(it);
            continue;
        }

        if (dPage_entry->backup_image == nullptr) {
            // Appended pages have no pre-transaction image. Explicit rollback invalidates any
            // outstanding pointers to them immediately, so callers must not touch or unref
            // those stale handles after this point.
            pCache->force_remove(page_num);
        } else if (cached_page->refs_num > 0) {
            std::memcpy(cached_page->data.data(), dPage_entry->backup_image, PAGE_SIZE);
            cached_page->is_dirty = false;
            cached_page->need_flushing = false;
        } else {
            PCacheResult remove_result = pCache->remove(page_num);
            assert(remove_result == PCacheResult::Success);
        }

        destroy_dirty_page_entry(dPage_entry);
        it = dirty_pages.erase(it);
    }

    if (txn_init_header_valid) {
        db_header = txn_init_header;
    }
    write_txn_state = WriteTxnState::None;
    txn_init_header_valid = false;
    return PagerResult::Success;
}

PagerResult Pager::purge_cache() {
    // This is only safe when we are not in the middle of our own write transaction.
    // If the file change counter moved forward, any older cached pages must be thrown away.
    assert(write_txn_state == WriteTxnState::None);
    assert(dirty_pages.empty());
    assert(pCache->len() == pCache->unpinned_len());

    delete pCache;
    pCache = new PCache();
    return PagerResult::Success;
}

PagerResult Pager::create_new_database_file() {
    // Create a DB file, initialize the db header and write it to disk
    DBHeader initial_header;
    initial_header.db_page_count = 1;

    // Serialize the initial DB header
    PageV2 header_page;
    V2PageCodec::initialize(
        header_page.data,
        DB_HEADER_PAGE_NUM,
        V2PageKind::DatabaseMetadata);
    DBHeaderCodec::serialize_DBHeader(initial_header, page_payload(&header_page));
    V2PageCodec::update_checksum(header_page.data);

    int create_fd = -1;
    try {
        create_fd = disk::open_file(db_name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    } catch (const std::exception &) {
        return PagerResult::OpenDbFailed;
    }

    try {
        // Write the header to disk
        std::span<const char> header_page_span(header_page.data);
        disk::write_exact_at(create_fd, header_page_span, 0);
    } catch (const std::exception &) {
        close_fd_if_open(create_fd);
        return PagerResult::OpenDbFailed;
    }

    // Make sure to sync it on disk
    try {
        disk::sync_file_to_disk_fd(create_fd);
    } catch (const std::exception &) {
        close_fd_if_open(create_fd);
        return PagerResult::OpenDbFailed;
    }

    if (!close_fd_if_open(create_fd)) {
        return PagerResult::OpenDbFailed;
    }

    // Set the db header to the one we just created
    db_header = initial_header;
    return PagerResult::Success;
}

PagerResult Pager::load_db_header_from_disk() {
    // TODO: Should we acquire a shared lock here?
    PageV2 header_page;
    try {
        std::span<char> header_page_span(header_page.data);
        disk::read_exact_at(db_fd, header_page_span, 0);
    } catch (const std::exception &) {
        return PagerResult::DbReadFailed;
    }

    if (V2PageCodec::validate(header_page.data) != V2PageCodecResult::Success ||
        V2PageCodec::page_num(header_page.data) != DB_HEADER_PAGE_NUM ||
        V2PageCodec::page_kind(header_page.data) != V2PageKind::DatabaseMetadata) {
        return PagerResult::DbHeaderCorrupt;
    }

    DBHeader loaded_header;
    DBHeaderCodec::deserialize_DBHeader(loaded_header, page_payload(&header_page));
    if (!DBHeaderCodec::validate_DBHeader(loaded_header)) {
        return PagerResult::DbHeaderCorrupt;
    }
    if (loaded_header.db_page_count == 0) {
        return PagerResult::DbHeaderCorrupt;
    }

    std::size_t file_size = 0;
    try {
        file_size = disk::file_size(db_fd);
    } catch (const std::exception &) {
        return PagerResult::DbReadFailed;
    }
    if (file_size % PAGE_SIZE != 0) return PagerResult::DbHeaderCorrupt;
    if (loaded_header.db_page_count > file_size / PAGE_SIZE) return PagerResult::DbHeaderCorrupt;

    // Before assigning the db_header to the loaded one
    // let's check first the file change count. 
    // If the loaded has a higher file change count, we purge the cache
    if (db_header.db_page_count != 0 && db_header.file_change_counter < loaded_header.file_change_counter) {
        PagerResult purge_result = purge_cache();
        if (purge_result != PagerResult::Success) return purge_result;
    }
    db_header = loaded_header;
    
    // if page 0 already happens to be cached, update that cached copy so it matches the disk/header we just loaded
    PageV2 *cached_header_page = pCache->get(DB_HEADER_PAGE_NUM);
    if (cached_header_page) {
        std::memcpy(cached_header_page->data.data(), header_page.data.data(), PAGE_SIZE);
        cached_header_page->is_dirty = false;
        cached_header_page->need_flushing = false;
    }

    return PagerResult::Success;
}

PagerResult Pager::ensure_header_page_loaded(PageV2 *&page) {
    // Make sure the header is loaded. If it's not found in cache,
    // load it from disk. This is necessary for write transaction
    // as we have to mark the header as dirty.
    // if we mutate db_header in memory, page 0 must also be present in cache and marked dirty
    return get_or_load_page(DB_HEADER_PAGE_NUM, page);
}

PagerResult Pager::get_or_load_page(int page_num, PageV2 *&page) {
    // Given a reference to a pointer, get the poitner to the page struct
    // either from cache, or through reading disk
    page = pCache->get(page_num);
    if (page) return PagerResult::Success;

    PagerReadPageResult read_result = read_page_from_disk(page_num);
    if (read_result.status != PagerResult::Success) {
        return read_result.status;
    }

    // If we had to load from disk, make sure to add to cache
    page = read_result.page;
    return cache_put(page);
}

PagerResult Pager::ensure_transaction_started() {
    // At the beginning of a transaction, we want to save the initial db header
    // and then modify the db_header file_change_counter, and mark the header page as dirty
    // We need to have the header in cache to ensure this
    // We also want to do that once at the start of the transaction. That's it
    // So if we already started the transaction, we already did that, and just return
    if (write_txn_state != WriteTxnState::None) return PagerResult::Success;


    // If this is the first time we are modifying the db in the transaction,
    // Make sure to load it into the cache if it's not already loaded.
    PageV2 *header_page = nullptr;
    PagerResult header_load_result = ensure_header_page_loaded(header_page);
    if (header_load_result != PagerResult::Success) return header_load_result;

    // Save its initial state
    txn_init_header = db_header;
    txn_init_header_valid = true; // txn_init_header now contains a real transaction-start snapshot.
    write_txn_state = WriteTxnState::DirtyInMemory; // Mark the transaction state as dirty in memory since we are about to modify the header

    PagerResult dirty_header_result = mark_loaded_page_dirty(header_page); // Mark the header as dirty
    if (dirty_header_result != PagerResult::Success) return dirty_header_result;

    // Increment the file counter
    db_header.file_change_counter++;
    sync_db_header_to_cached_header_page(); // make sure to sync those changes to the db header bytes in the cached page 0 buffer
    return PagerResult::Success;
}

PagerResult Pager::mark_header_dirty_for_mutation() {
    // Make sure the transaction started. 
    PagerResult txn_result = ensure_transaction_started();
    if (txn_result != PagerResult::Success) return txn_result;
    
    // Make sure the header is loaded into cache and mark it as dirty
    PageV2 *header_page = nullptr;
    PagerResult header_load_result = ensure_header_page_loaded(header_page);
    if (header_load_result != PagerResult::Success) return header_load_result;

    return mark_loaded_page_dirty(header_page);
}

PagerResult Pager::mark_loaded_page_dirty(PageV2 *page) {
    // If it's already dirty, return nothing
    if (page->is_dirty && page->need_flushing) return PagerResult::Success;

    // If it was dirty, and spilled over, but now getting modifed again
    // mark it as need flushing
    if (page->is_dirty && !page->need_flushing) {
        assert(write_txn_state != WriteTxnState::None);
        page->need_flushing = true;
        return PagerResult::Success;
    }

    // Set the dirty and flushing flags to true
    page->is_dirty = true;
    page->need_flushing = true;

    // Make sure to add it to the dirty list
    auto it = dirty_pages.find(page->page_num);
    if (it == dirty_pages.end()) {
        DirtyPageEntry *new_entry = new DirtyPageEntry();
        new_entry->backup_image = new char[PAGE_SIZE];
        std::memcpy(new_entry->backup_image, page->data.data(), PAGE_SIZE);
        new_entry->page = page;
        dirty_pages[page->page_num] = new_entry;
    } else {
        it->second->page = page;
    }
    return PagerResult::Success;
}

void Pager::sync_db_header_to_cached_header_page() {
    // Make sure that the modified in-memory db header is synced with the bytes in  in-memory cache buffer
    PageV2 *header_page = pCache->get(DB_HEADER_PAGE_NUM);
    assert(header_page != nullptr);
    DBHeaderCodec::serialize_DBHeader(db_header, page_payload(header_page));
    V2PageCodec::update_checksum(header_page->data);
}

void Pager::read_freelist_links(PageV2 *page, std::uint32_t &next_page_num, std::uint32_t &prev_page_num) {
    // Trivial
    const char *payload = page_payload(page);
    next_page_num = get_u32_be(&payload[FREELIST_NEXT_OFFSET]);
    prev_page_num = get_u32_be(&payload[FREELIST_PREV_OFFSET]);
}

void Pager::write_freelist_links(PageV2 *page, std::uint32_t next_page_num, std::uint32_t prev_page_num) {
    // Trivial
    V2PageCodec::set_page_kind(page->data, V2PageKind::Freelist);
    char *payload = page_payload(page);
    put_u32_be(&payload[FREELIST_NEXT_OFFSET], next_page_num);
    put_u32_be(&payload[FREELIST_PREV_OFFSET], prev_page_num);
}

Pager::PagerReadPageResult Pager::read_page_from_disk(int page_num) {
    // We need too check if the cache has enough space for a read
    // Which means either pCache->length < capacity or there's a free
    // unpinned page (not dirty). Instead of handling this here, it should be handled in 
    // the cache. Would be nice tho if there's a fast way to check if there's
    // an unpinned page that is not dirty. For the early versions, we are going to throw an error
    // when the only available pages are dirty so it's important in this case that we dont even 
    // waste time trying to fetch from disk the page. but later on, the cache itself will handle
    // flushing the dirty unpinned page so doesn't matter.
    PagerReadPageResult result;

    if (page_num < 0 || page_num >= static_cast<int>(db_header.db_page_count)) {
        result.status = PagerResult::PageOutOfRange;
        return result;
    }

    std::streamoff offset = static_cast<std::streamoff>(page_num) * static_cast<std::streamoff>(PAGE_SIZE);
    PageV2 *page = nullptr;
    try {
        page = new PageV2();

        std::span<char> buffer_span(page->data);
        disk::read_exact_at(db_fd, buffer_span, offset);
    } catch (const std::exception &) {
        delete page;
        result.status = PagerResult::DbReadFailed;
        return result;
    }
    if (V2PageCodec::validate(page->data) != V2PageCodecResult::Success ||
        V2PageCodec::page_num(page->data) != static_cast<std::uint32_t>(page_num)) {
        delete page;
        result.status = PagerResult::PageCorrupt;
        return result;
    }
    if ((page_num == DB_HEADER_PAGE_NUM) !=
        (V2PageCodec::page_kind(page->data) == V2PageKind::DatabaseMetadata)) {
        delete page;
        result.status = PagerResult::PageCorrupt;
        return result;
    }

    page->page_num = static_cast<std::uint32_t>(page_num);
    result.page = page;
    return result;
}

void Pager::handle_cache_eviction(const PCacheEviction &eviction) {
    // PCache may evict a page object to make space
    // if that evicted page was dirty, pager may still have a DirtyPageEntry for it in dirty_pages
    // once the page has been flushed and evicted, that dirty-tracking entry may no longer be needed
    // so this function removes that stale dirty entry
    if (!eviction.happened || !eviction.was_dirty) return;

    auto it = dirty_pages.find(eviction.page_num);
    if (it == dirty_pages.end()) return;

    // Page was dirty and was already flushed so no worries gang
    // destroy that mf
    destroy_dirty_page_entry(it->second);
    dirty_pages.erase(it);
}

PagerResult Pager::cache_put(PageV2 *page) {
    PCachePutResult put_result = pCache->put(page);
    // Handle cache evictions for dirty pages if we succeded
    if (put_result.status == PCacheResult::Success) {
        handle_cache_eviction(put_result.eviction);
        return PagerResult::Success;
    }

    // If there's no page to kick out, not even a dirty one
    // we need to overspill the cache to disk then
    if (put_result.status == PCacheResult::DirtyFlush) {
        PagerResult phase_one_result = commit_phase_one();
        if (phase_one_result != PagerResult::Success) {
            // Failed to commit phase one. abort the fucking transaction
            delete page;
            return phase_one_result;
        }

        // Now try to put and handle cache eviction again
        put_result = pCache->put(page);
        if (put_result.status == PCacheResult::Success) {
            handle_cache_eviction(put_result.eviction);
            return PagerResult::Success;
        }
    }

    // If after all of this we still failed to put, i dont know where to go from here
    delete page;
    // TODO: In the future, we will expand the cache temporarily
    if (put_result.status == PCacheResult::NoVictim) {
        return PagerResult::NoEvictablePage;
    }
    if (put_result.status == PCacheResult::WalPending) {
        return PagerResult::NoEvictablePage;
    }
    // If it was dirty flush and we still failed to overspill, then we are cooked
    if (put_result.status == PCacheResult::DirtyFlush) {
        return PagerResult::CacheSpillFailed;
    }

    return PagerResult::CacheSpillFailed;
}
