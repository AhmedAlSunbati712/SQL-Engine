#include <Pager.h>
#include <DBHeaderCodec.h>
#include <DiskIO.h>
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
}

Pager::~Pager() {
    close_fd_if_open(db_fd);
    delete pCache;
}

PagerResult Pager::open(std::string db_file) {
    std::lock_guard lock(mutex_);
    assert(!is_open);
    if (is_open) return PagerResult::OpenDbFailed;

    // The WAL path is the only mutation path Pager implements. A caller must
    // attach a Log before open() so every subsequent mutation can enforce
    // WAL-before-data.
    if (!log_) return PagerResult::LogNotAttached;

    db_name = std::move(db_file);

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
                return PagerResult::DbHeaderCorrupt;
            }
        } catch (const std::exception &) {
            db_name.clear();
            return PagerResult::OpenDbFailed;
        }
    }
    // Now, we are sure there's a db file on disk that is valid
    // open the database file
    try {
        db_fd = disk::open_file(db_name, O_RDWR);
    } catch (const std::exception &) {
        db_name.clear();
        return PagerResult::OpenDbFailed;
    }

    // Set the is_open state
    is_open = true;

    // Now, we need to load the header into our Pager state
    // so it can be referenced by the different operations (allocate_page, free_page for example)
    PagerResult header_result = load_db_header_from_disk();
    if (header_result != PagerResult::Success) {
        // If we cant load the header, something went wrong.
        close_fd_if_open(db_fd);
        db_name.clear();
        is_open = false;
        return header_result;
    }

    return PagerResult::Success;
}

PagerGetResult Pager::get(int page_num) {
    std::lock_guard lock(mutex_);
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

    if (page_num >= static_cast<int>(db_header.db_page_count)) {
        result.status = PagerResult::PageOutOfRange;
        return result;
    }

    PageV2 *page = nullptr;
    result.status = get_or_load_page(page_num, page);
    if (result.status != PagerResult::Success) return result;
    page->refs_num++;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    result.page = page;
    return result;
}

PagerAllocateResult Pager::allocate_page(
    BTreeOperation& operation,
    V2PageKind kind
) {
    PagerAllocateResult result{};
    if (kind != V2PageKind::BTreeLeaf && kind != V2PageKind::BTreeInternal) {
        result.status = PagerResult::InvalidPageKind;
        return result;
    }

    // Page zero serializes page-number allocation and freelist changes. Do not
    // hold the Pager mutex while waiting for this logical latch.
    operation.lock_exclusive(DB_HEADER_PAGE_NUM);

    std::uint32_t freelist_head_page_num = FREELIST_NULL_PAGE_NUM;
    {
        std::lock_guard lock(mutex_);
        if (!is_open) {
            result.status = PagerResult::DatabaseNotOpen;
            return result;
        }
        freelist_head_page_num = db_header.freelist_page_count > 0
            ? db_header.freelist_head_page_num
            : FREELIST_NULL_PAGE_NUM;
    }

    if (freelist_head_page_num != FREELIST_NULL_PAGE_NUM) {
        operation.lock_exclusive(freelist_head_page_num);

        std::uint32_t next_free_page_num = FREELIST_NULL_PAGE_NUM;
        {
            std::lock_guard lock(mutex_);
            PageV2 *head = nullptr;
            result.status = get_or_load_page(
                static_cast<int>(freelist_head_page_num), head);
            if (result.status != PagerResult::Success) return result;
            std::uint32_t ignored_previous = FREELIST_NULL_PAGE_NUM;
            read_freelist_links(head, next_free_page_num, ignored_previous);
        }
        if (next_free_page_num != FREELIST_NULL_PAGE_NUM) {
            operation.lock_exclusive(next_free_page_num);
        }

        std::lock_guard lock(mutex_);
        PageV2 *header = nullptr;
        PageV2 *head = nullptr;
        PageV2 *next = nullptr;
        result.status = ensure_header_page_loaded(header);
        if (result.status != PagerResult::Success) return result;
        result.status = get_or_load_page(
            static_cast<int>(freelist_head_page_num), head);
        if (result.status != PagerResult::Success) return result;
        if (next_free_page_num != FREELIST_NULL_PAGE_NUM) {
            result.status = get_or_load_page(
                static_cast<int>(next_free_page_num), next);
            if (result.status != PagerResult::Success) return result;
        }

        // Every affected frame becomes non-evictable before any freelist or
        // metadata bytes are changed.
        result.status = mark_loaded_page_wal_pending(operation, header);
        if (result.status != PagerResult::Success) return result;
        result.status = mark_loaded_page_wal_pending(operation, head);
        if (result.status != PagerResult::Success) return result;
        if (next) {
            result.status = mark_loaded_page_wal_pending(operation, next);
            if (result.status != PagerResult::Success) return result;

            std::uint32_t next_next = FREELIST_NULL_PAGE_NUM;
            std::uint32_t ignored_previous = FREELIST_NULL_PAGE_NUM;
            read_freelist_links(next, next_next, ignored_previous);
            write_freelist_links(next, next_next, FREELIST_NULL_PAGE_NUM);
        }

        db_header.freelist_head_page_num = next_free_page_num;
        db_header.freelist_page_count--;
        sync_db_header_to_cached_header_page();

        V2PageCodec::initialize(
            head->data,
            freelist_head_page_num,
            kind);
        assert(head->refs_num == 0);
        head->refs_num = 1;
        pCache->pin_page(static_cast<int>(freelist_head_page_num));

        result.page_num = static_cast<int>(freelist_head_page_num);
        result.page = head;
        result.effects.push_back(page_effect(PageEffectKind::Write, header));
        if (next) result.effects.push_back(page_effect(PageEffectKind::Write, next));
        result.effects.push_back(page_effect(PageEffectKind::Allocate, head));
        return result;
    }

    std::uint32_t new_page_num = 0;
    {
        std::lock_guard lock(mutex_);
        new_page_num = db_header.db_page_count;
    }
    operation.lock_exclusive(new_page_num);

    std::lock_guard lock(mutex_);
    PageV2 *header = nullptr;
    result.status = ensure_header_page_loaded(header);
    if (result.status != PagerResult::Success) return result;

    // Cache the new frame before publishing the larger page count. This keeps
    // a cache-capacity failure from exposing metadata for a missing page.
    PageV2 *new_page = make_page(static_cast<int>(new_page_num), kind);
    new_page->refs_num = 1;
    new_page->is_dirty = true;
    new_page->wal_pending = true;
    new_page->need_flushing = true;
    wal_dirty_pages_.insert(new_page_num);
    result.status = cache_put(new_page);
    if (result.status != PagerResult::Success) {
        wal_dirty_pages_.erase(new_page_num);
        return result;
    }

    result.status = mark_loaded_page_wal_pending(operation, header);
    if (result.status != PagerResult::Success) {
        wal_dirty_pages_.erase(new_page_num);
        pCache->force_remove(static_cast<int>(new_page_num));
        return result;
    }
    db_header.db_page_count++;
    sync_db_header_to_cached_header_page();

    result.page_num = static_cast<int>(new_page_num);
    result.page = new_page;
    result.effects.push_back(page_effect(PageEffectKind::Write, header));
    result.effects.push_back(page_effect(PageEffectKind::Allocate, new_page));
    return result;
}

PagerMutationResult Pager::free_page(
    BTreeOperation& operation,
    std::uint32_t page_num
) {
    PagerMutationResult result{};

    // Freeing always changes page zero and the freed page. Acquire both before
    // consulting Pager state so no mutex is held while waiting for a tree latch.
    operation.lock_exclusive(DB_HEADER_PAGE_NUM);
    operation.lock_exclusive(page_num);

    std::uint32_t old_head_page_num = FREELIST_NULL_PAGE_NUM;
    {
        std::lock_guard lock(mutex_);
        if (!is_open) {
            result.status = PagerResult::DatabaseNotOpen;
            return result;
        }
        if (page_num == DB_HEADER_PAGE_NUM || page_num >= db_header.db_page_count) {
            result.status = PagerResult::PageOutOfRange;
            return result;
        }
        old_head_page_num = db_header.freelist_head_page_num;
    }
    if (old_head_page_num != FREELIST_NULL_PAGE_NUM) {
        operation.lock_exclusive(old_head_page_num);
    }

    std::lock_guard lock(mutex_);
    PageV2 *header = nullptr;
    PageV2 *freed = nullptr;
    PageV2 *old_head = nullptr;
    result.status = ensure_header_page_loaded(header);
    if (result.status != PagerResult::Success) return result;
    result.status = get_or_load_page(static_cast<int>(page_num), freed);
    if (result.status != PagerResult::Success) return result;
    if (old_head_page_num != FREELIST_NULL_PAGE_NUM) {
        result.status = get_or_load_page(static_cast<int>(old_head_page_num), old_head);
        if (result.status != PagerResult::Success) return result;
    }

    result.status = mark_loaded_page_wal_pending(operation, header);
    if (result.status != PagerResult::Success) return result;
    result.status = mark_loaded_page_wal_pending(operation, freed);
    if (result.status != PagerResult::Success) return result;
    if (old_head) {
        result.status = mark_loaded_page_wal_pending(operation, old_head);
        if (result.status != PagerResult::Success) return result;

        std::uint32_t old_next = FREELIST_NULL_PAGE_NUM;
        std::uint32_t ignored_previous = FREELIST_NULL_PAGE_NUM;
        read_freelist_links(old_head, old_next, ignored_previous);
        write_freelist_links(old_head, old_next, page_num);
    }

    write_freelist_links(freed, old_head_page_num, FREELIST_NULL_PAGE_NUM);
    db_header.freelist_head_page_num = page_num;
    db_header.freelist_page_count++;
    sync_db_header_to_cached_header_page();

    result.effects.push_back(page_effect(PageEffectKind::Write, header));
    if (old_head) result.effects.push_back(page_effect(PageEffectKind::Write, old_head));
    result.effects.push_back(page_effect(PageEffectKind::Free, freed));
    return result;
}

PagerResult Pager::begin_wal_write(
    BTreeOperation& operation,
    std::uint32_t page_num
) {
    std::lock_guard lock(mutex_);
    if (!is_open) return PagerResult::DatabaseNotOpen;

    PageV2 *page = nullptr;
    PagerResult load_result = get_or_load_page(static_cast<int>(page_num), page);
    if (load_result != PagerResult::Success) return load_result;

    return mark_loaded_page_wal_pending(operation, page);
}

PagerResult Pager::ref_page(int page_num) {
    std::lock_guard lock(mutex_);
    if (!is_open) return PagerResult::DatabaseNotOpen;

    assert(page_num > DB_HEADER_PAGE_NUM);
    if (page_num <= DB_HEADER_PAGE_NUM) return PagerResult::PageOutOfRange;

    PageV2 *page = pCache->get(page_num);
    if (!page) return PagerResult::PageNotCached;
    page->refs_num++;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    return PagerResult::Success;
}

PagerResult Pager::unref_page(int page_num) {
    std::lock_guard lock(mutex_);
    if (!is_open) return PagerResult::DatabaseNotOpen;

    PageV2 *page = pCache->get(page_num);
    assert(page != nullptr);

    page->refs_num--;
    if (page->refs_num == 0) pCache->unpin_page(page_num);

    return PagerResult::Success;
}

PagerGetRootResult Pager::get_btree_root() {
    std::lock_guard lock(mutex_);
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
    operation.lock_exclusive(DB_HEADER_PAGE_NUM);
    if (root_page_num != 0) operation.lock_exclusive(root_page_num);

    PagerMutationResult result{};
    std::lock_guard lock(mutex_);
    if (!is_open) {
        result.status = PagerResult::DatabaseNotOpen;
        return result;
    }
    if (root_page_num >= db_header.db_page_count && root_page_num != 0) {
        result.status = PagerResult::PageOutOfRange;
        return result;
    }

    PageV2 *header = nullptr;
    result.status = ensure_header_page_loaded(header);
    if (result.status != PagerResult::Success) return result;
    result.status = mark_loaded_page_wal_pending(operation, header);
    if (result.status != PagerResult::Success) return result;

    db_header.btree_root_page_num = root_page_num;
    sync_db_header_to_cached_header_page();
    result.effects.push_back(page_effect(PageEffectKind::Write, header));
    return result;
}

PagerResult Pager::install_page_lsn(
    BTreeOperation& operation,
    std::uint32_t page_num,
    Lsn lsn
) {
    std::lock_guard lock(mutex_);
    if (!is_open) return PagerResult::DatabaseNotOpen;
    if (lsn == 0) return PagerResult::PageCorrupt;

    // The page image cannot be published under an LSN unless this operation
    // still owns the exclusive logical latch that protected its modification.
    const std::optional<PageLatchMode> mode = operation.latch_mode(page_num);
    if (!mode || *mode != PageLatchMode::Exclusive) {
        return PagerResult::Busy;
    }

    // A pending frame cannot be evicted, so it must still be resident. Looking
    // it up directly also avoids refreshing page-zero metadata from disk while
    // the current WAL action has newer, not-yet-flushed metadata in memory.
    PageV2 *page = pCache->get(static_cast<int>(page_num));
    if (!page) return PagerResult::PageNotCached;

    // Every changed frame must become pending before its first modified byte.
    // Missing this state would allow an unlogged page to be flushed or evicted.
    if (!page->wal_pending) {
        return PagerResult::CacheSpillFailed;
    }

    V2PageCodec::set_page_lsn(page->data, lsn);
    V2PageCodec::update_checksum(page->data);
    page->is_dirty = true;
    page->wal_pending = false;

    return PagerResult::Success;
}

PagerResult Pager::mark_loaded_page_wal_pending(
    BTreeOperation& operation,
    PageV2 *page
) {
    // A frame may become non-evictable only while the operation still owns
    // the exclusive logical latch protecting its unlogged bytes.
    const std::optional<PageLatchMode> mode = operation.latch_mode(page->page_num);
    if (!mode || *mode != PageLatchMode::Exclusive) {
        return PagerResult::Busy;
    }

    // Repeated preparation is harmless and must not replace the original
    // pending state while one operation modifies a page more than once.
    page->is_dirty = true;
    page->wal_pending = true;
    page->need_flushing = true;
    wal_dirty_pages_.insert(page->page_num);
    return PagerResult::Success;
}

PagerResult Pager::flush_wal_page(std::uint32_t page_num) {
    PageV2 *page = pCache->get(static_cast<int>(page_num));
    if (!page) {
        wal_dirty_pages_.erase(page_num);
        return PagerResult::Success;
    }
    if (page->wal_pending) return PagerResult::CacheSpillFailed;
    if (!page->is_dirty) {
        page->need_flushing = false;
        wal_dirty_pages_.erase(page_num);
        return PagerResult::Success;
    }

    const Lsn page_lsn = V2PageCodec::page_lsn(page->data);
    if (page_lsn == 0 || !log_) return PagerResult::CacheSpillFailed;

    try {
        // WAL reaches stable storage before its page is allowed to overwrite
        // the database file. The database page itself may be synchronized later.
        log_->sync_through(page_lsn);
        V2PageCodec::update_checksum(page->data);
        disk::write_exact_at(
            db_fd,
            std::span<const char>(page->data),
            static_cast<std::streamoff>(page_num) * static_cast<std::streamoff>(PAGE_SIZE));
    } catch (...) {
        return PagerResult::DbWriteFailed;
    }

    page->is_dirty = false;
    page->need_flushing = false;
    wal_dirty_pages_.erase(page_num);
    return PagerResult::Success;
}

PagerResult Pager::flush_wal_pages() {
    std::lock_guard lock(mutex_);
    if (!is_open) return PagerResult::DatabaseNotOpen;

    // Work from a copy because each successful flush removes its page number
    // from the authoritative dirty set.
    const std::vector<std::uint32_t> pages(
        wal_dirty_pages_.begin(),
        wal_dirty_pages_.end());
    for (std::uint32_t page_num : pages) {
        PagerResult result = flush_wal_page(page_num);
        if (result != PagerResult::Success) return result;
    }

    try {
        disk::sync_file_to_disk_fd(db_fd);
    } catch (...) {
        return PagerResult::DbFlushFailed;
    }

    return PagerResult::Success;
}


PagerResult Pager::purge_cache() {
    // This is only safe when no page is still referenced.
    // If the file change counter moved forward, any older cached pages must be thrown away.
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
    // PCache may evict a page object to make space. Once an evicted page has
    // been flushed, its WAL-dirty tracking entry is no longer needed.
    if (!eviction.happened || !eviction.was_dirty) return;

    wal_dirty_pages_.erase(static_cast<std::uint32_t>(eviction.page_num));
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
        PagerResult flush_result = flush_wal_page(
            static_cast<std::uint32_t>(put_result.eviction.page_num));
        if (flush_result != PagerResult::Success) {
            // Failed to commit phase one. abort the fucking transaction
            delete page;
            return flush_result;
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
