#include <Pager.h>
#include <DBHeaderCodec.h>
#include <DiskIO.h>
#include <JournalCodec.h>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <span>
#include <unordered_set>

namespace {

constexpr std::uint32_t FREELIST_NULL_PAGE_NUM = 0;
constexpr std::size_t FREELIST_NEXT_OFFSET = 0;
constexpr std::size_t FREELIST_PREV_OFFSET = 4;

void destroy_dirty_page_entry(DirtyPageEntry *entry) {
    delete[] entry->backup_image;
    delete entry;
}

Page *make_page(int page_num) {
    Page *page = new Page();
    std::memset(page->data, 0, PAGE_SIZE);
    page->page_num = page_num;
    page->refs_num = 0;
    page->is_dirty = false;
    page->need_flushing = false;
    return page;
}

} // namespace

Pager::Pager() {
    pCache = new PCache();
}

Pager::~Pager() {
    for (auto it = dirty_pages.begin(); it != dirty_pages.end(); ) {
        destroy_dirty_page_entry(it->second);
        it = dirty_pages.erase(it);
    }

    if (dbFile_handler.is_open()) dbFile_handler.close();
    delete pCache;
}

PagerResult Pager::open(std::string db_file) {
    assert(!is_open);
    if (is_open) return PagerResult::OpenDbFailed;

    db_name = std::move(db_file);
    jFile_name = db_name + "_journal";
    write_txn_state = WriteTxnState::None;
    txn_init_header_valid = false;

    bool db_exists = false;
    try {
        db_exists = std::filesystem::exists(db_name);
    } catch (const std::exception &) {
        return PagerResult::OpenDbFailed;
    }

    if (!db_exists) {
        PagerResult create_result = create_new_database_file();
        if (create_result != PagerResult::Success) {
            db_name.clear();
            jFile_name.clear();
            return create_result;
        }
    } else {
        try {
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

    dbFile_handler.open(db_name, std::ios::in | std::ios::out | std::ios::binary);
    if (!dbFile_handler.is_open() || dbFile_handler.fail()) {
        db_name.clear();
        jFile_name.clear();
        return PagerResult::OpenDbFailed;
    }

    is_open = true;

    PagerResult recovery_result = maybe_recover_hot_journal();
    if (recovery_result != PagerResult::Success) {
        if (dbFile_handler.is_open()) dbFile_handler.close();
        db_name.clear();
        jFile_name.clear();
        is_open = false;
        write_txn_state = WriteTxnState::None;
        txn_init_header_valid = false;
        return recovery_result;
    }

    PagerResult header_result = load_db_header_from_disk();
    if (header_result != PagerResult::Success) {
        if (dbFile_handler.is_open()) dbFile_handler.close();
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

    assert(page_num > DB_HEADER_PAGE_NUM);
    if (page_num <= DB_HEADER_PAGE_NUM || page_num >= static_cast<int>(db_header.db_page_count)) {
        result.status = PagerResult::PageOutOfRange;
        return result;
    }

    PagerResult recovery_result = maybe_recover_hot_journal();
    if (recovery_result != PagerResult::Success) {
        result.status = recovery_result;
        return result;
    }

    Page *page = nullptr;
    PagerResult load_result = get_or_load_page(page_num, page);
    if (load_result != PagerResult::Success) {
        result.status = load_result;
        return result;
    }

    page->refs_num += 1;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    result.data = page->data;
    return result;
}

PagerAllocateResult Pager::allocate_page() {
    PagerAllocateResult result;
    if (!is_open) {
        result.status = PagerResult::DatabaseNotOpen;
        return result;
    }

    PagerResult recovery_result = maybe_recover_hot_journal();
    if (recovery_result != PagerResult::Success) {
        result.status = recovery_result;
        return result;
    }

    if (db_header.freelist_page_count > 0) {
        int freelist_head_page_num = static_cast<int>(db_header.freelist_head_page_num);
        assert(freelist_head_page_num > DB_HEADER_PAGE_NUM);
        assert(freelist_head_page_num < static_cast<int>(db_header.db_page_count));

        Page *freelist_head_page = nullptr;
        PagerResult load_head_result = get_or_load_page(freelist_head_page_num, freelist_head_page);
        if (load_head_result != PagerResult::Success) {
            result.status = load_head_result;
            return result;
        }

        std::uint32_t next_free_page_num = FREELIST_NULL_PAGE_NUM;
        std::uint32_t prev_free_page_num = FREELIST_NULL_PAGE_NUM;
        read_freelist_links(freelist_head_page, next_free_page_num, prev_free_page_num);
        assert(prev_free_page_num == FREELIST_NULL_PAGE_NUM);

        Page *next_free_page = nullptr;
        if (next_free_page_num != FREELIST_NULL_PAGE_NUM) {
            assert(next_free_page_num < db_header.db_page_count);
            PagerResult load_next_result = get_or_load_page(static_cast<int>(next_free_page_num), next_free_page);
            if (load_next_result != PagerResult::Success) {
                result.status = load_next_result;
                return result;
            }
        }

        PagerResult header_result = mark_header_dirty_for_mutation();
        if (header_result != PagerResult::Success) {
            result.status = header_result;
            return result;
        }

        PagerResult dirty_head_result = mark_loaded_page_dirty(freelist_head_page);
        if (dirty_head_result != PagerResult::Success) {
            result.status = dirty_head_result;
            return result;
        }

        if (next_free_page) {
            PagerResult dirty_next_result = mark_loaded_page_dirty(next_free_page);
            if (dirty_next_result != PagerResult::Success) {
                result.status = dirty_next_result;
                return result;
            }

            std::uint32_t next_next_page_num = FREELIST_NULL_PAGE_NUM;
            std::uint32_t next_prev_page_num = FREELIST_NULL_PAGE_NUM;
            read_freelist_links(next_free_page, next_next_page_num, next_prev_page_num);
            write_freelist_links(next_free_page, next_next_page_num, FREELIST_NULL_PAGE_NUM);
        }

        db_header.freelist_head_page_num = next_free_page_num;
        db_header.freelist_page_count--;
        sync_db_header_to_cached_header_page();

        std::memset(freelist_head_page->data, 0, PAGE_SIZE);
        assert(freelist_head_page->refs_num == 0);
        freelist_head_page->refs_num = 1;
        pCache->pin_page(freelist_head_page_num);

        result.page_num = freelist_head_page_num;
        result.data = freelist_head_page->data;
        return result;
    }

    PagerResult header_result = mark_header_dirty_for_mutation();
    if (header_result != PagerResult::Success) {
        result.status = header_result;
        return result;
    }

    int new_page_num = static_cast<int>(db_header.db_page_count);
    db_header.db_page_count++;
    sync_db_header_to_cached_header_page();

    Page *new_page = make_page(new_page_num);
    new_page->refs_num = 1;

    PagerResult cache_result = cache_put(new_page);
    if (cache_result != PagerResult::Success) {
        db_header.db_page_count--;
        sync_db_header_to_cached_header_page();
        result.status = cache_result;
        return result;
    }

    new_page->is_dirty = true;
    new_page->need_flushing = true;

    auto it = dirty_pages.find(new_page_num);
    assert(it == dirty_pages.end());
    DirtyPageEntry *new_entry = new DirtyPageEntry();
    new_entry->page = new_page;
    dirty_pages[new_page_num] = new_entry;

    result.page_num = new_page_num;
    result.data = new_page->data;
    return result;
}

PagerResult Pager::free_page(int page_num) {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    assert(page_num > DB_HEADER_PAGE_NUM);
    if (page_num <= DB_HEADER_PAGE_NUM || page_num >= static_cast<int>(db_header.db_page_count)) {
        return PagerResult::PageOutOfRange;
    }

    PagerResult recovery_result = maybe_recover_hot_journal();
    if (recovery_result != PagerResult::Success) return recovery_result;

    Page *page_to_free = nullptr;
    PagerResult load_page_result = get_or_load_page(page_num, page_to_free);
    if (load_page_result != PagerResult::Success) return load_page_result;

    Page *old_freelist_head = nullptr;
    if (db_header.freelist_head_page_num != FREELIST_NULL_PAGE_NUM) {
        assert(db_header.freelist_head_page_num != static_cast<std::uint32_t>(page_num));
        PagerResult load_head_result = get_or_load_page(static_cast<int>(db_header.freelist_head_page_num), old_freelist_head);
        if (load_head_result != PagerResult::Success) return load_head_result;
    }

    PagerResult header_result = mark_header_dirty_for_mutation();
    if (header_result != PagerResult::Success) return header_result;

    PagerResult dirty_page_result = mark_loaded_page_dirty(page_to_free);
    if (dirty_page_result != PagerResult::Success) return dirty_page_result;

    if (old_freelist_head) {
        PagerResult dirty_head_result = mark_loaded_page_dirty(old_freelist_head);
        if (dirty_head_result != PagerResult::Success) return dirty_head_result;

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
    if (page_num <= DB_HEADER_PAGE_NUM || page_num >= static_cast<int>(db_header.db_page_count)) {
        return PagerResult::PageOutOfRange;
    }

    PagerResult recovery_result = maybe_recover_hot_journal();
    if (recovery_result != PagerResult::Success) return recovery_result;

    Page *page = nullptr;
    PagerResult load_result = get_or_load_page(page_num, page);
    if (load_result != PagerResult::Success) return load_result;

    PagerResult txn_result = ensure_transaction_started();
    if (txn_result != PagerResult::Success) return txn_result;

    return mark_loaded_page_dirty(page);
}

PagerResult Pager::ref_page(int page_num) {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    Page *page = pCache->get(page_num);
    assert(page != nullptr);
    page->refs_num++;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    return PagerResult::Success;
}

PagerResult Pager::unref_page(int page_num) {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    Page *page = pCache->get(page_num);
    assert(page != nullptr);

    page->refs_num--;
    if (page->refs_num == 0) pCache->unpin_page(page_num);
    return PagerResult::Success;
}


// TODO: replace all seekp with seek_write_to
PagerResult Pager::commit_phase_one() {
    /**
     * Need a function call to invalidate dirty reads
     * Need a function call to invalidate current dirty reads and previous dirty reads (will need the need_to_flush journal flag back) and rollback the journal
     * N
     * should i define return constants so the user knows what to do?
     * Possible failure points:
     * # 1
     * flush the dirty pages to the journal
     * flush the journal header to disk.
     * now go through the same pages again and flush them to the db file
     */
    if (!is_open) return PagerResult::DatabaseNotOpen;

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

    bool journal_exists = false;
    PagerResult journal_check = journal_has_contents(journal_exists);
    if (journal_check != PagerResult::Success) return journal_check;
    JournalHeader jHeader;

    std::fstream jFile;
    if (!journal_exists) {
        jFile.open(jFile_name, std::ios::in | std::ios::out | std::ios::trunc | std::ios::binary);
        if (!jFile.is_open() || jFile.fail()) {
            return PagerResult::JournalCreateFailed;
        }

    } else {
        jFile.open(jFile_name, std::ios::in | std::ios::out | std::ios::binary);

        if (!jFile.is_open() || jFile.fail()) {
            return PagerResult::JournalOpenFailed;
        }

        char header_bytes[JOURNAL_HEADER_SIZE];
        try {
            disk::seek_read_to(jFile, 0);
            disk::read_exact(jFile, header_bytes);
        } catch (const std::exception &) {
            return PagerResult::JournalOpenFailed;
        }

        Journal::deserialize_jHeader(jHeader, header_bytes);

        bool valid_journal = Journal::validate_journal_header(jHeader);
        if (!valid_journal) {
            return PagerResult::JournalCorrupt;
        }
        jFile.seekp(0, std::ios::end); // Seek to the end of the file (after the most recent backup image appended)
    }

    std::streamoff curr_offset = 0;
    try {
        curr_offset = disk::get_curr_write_offset(jFile);
    } catch (const std::exception &) {
        return PagerResult::JournalHeaderWriteFailed;
    }
    curr_offset = align_to_page_boundary(curr_offset);

    assert(txn_init_header_valid);
    jHeader.nonce = Journal::generate_nonce();
    jHeader.init_db_page_count = txn_init_header.db_page_count;
    jHeader.page_count = 0;

    char jHeader_bytes[JOURNAL_HEADER_SIZE];
    try {
        disk::seek_write_to(jFile, curr_offset);
        Journal::serialize_jHeader(jHeader, jHeader_bytes);
        disk::write_exact(jFile, jHeader_bytes);
    } catch (const std::exception &) {
        return PagerResult::JournalHeaderWriteFailed;
    }

    try {
        disk::seek_write_to(jFile, curr_offset + static_cast<std::streamoff>(PAGE_SIZE));
    } catch (const std::exception &) {
        return PagerResult::JournalRecordWriteFailed;
    }

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
            disk::write_exact(jFile, jPage_record_bytes);
        } catch (const std::exception &) {
            return PagerResult::JournalRecordWriteFailed;
        }
    }

    jFile.flush();
    if (jFile.bad() || jFile.fail()) {
        return PagerResult::JournalFlushFailed;
    }
    try {
        disk::sync_file_to_disk(jFile_name);
    } catch (const std::exception &) {
        return PagerResult::JournalFlushFailed;
    }

    char jHeader_bytes_final[JOURNAL_HEADER_SIZE];
    try {
        disk::seek_write_to(jFile, curr_offset);
        Journal::serialize_jHeader(jHeader, jHeader_bytes_final);
        disk::write_exact(jFile, jHeader_bytes_final);
    } catch (const std::exception &) {
        return PagerResult::JournalHeaderWriteFailed;
    }

    jFile.flush();
    if (jFile.bad() || jFile.fail()) {
        return PagerResult::JournalFlushFailed;
    }
    try {
        disk::sync_file_to_disk(jFile_name);
    } catch (const std::exception &) {
        return PagerResult::JournalFlushFailed;
    }
    write_txn_state = WriteTxnState::JournalDurable;

    for (const auto &[page_num, dPage_entry] : dirty_pages) {
        std::span<char> new_page_data(dPage_entry->page->data);
        try {
            disk::seek_write_to(
                dbFile_handler,
                static_cast<std::streamoff>(page_num) * static_cast<std::streamoff>(PAGE_SIZE)
            );
            disk::write_exact(dbFile_handler, new_page_data);
        } catch (const std::exception &) {
            return PagerResult::DbWriteFailed;
        }
    }

    dbFile_handler.flush();
    if (dbFile_handler.bad() || dbFile_handler.fail()) {
        return PagerResult::DbFlushFailed;
    }
    try {
        disk::sync_file_to_disk(db_name);
    } catch (const std::exception &) {
        return PagerResult::DbFlushFailed;
    }

    for (auto &[page_num, dPage_entry] : dirty_pages) {
        (void)page_num;
        dPage_entry->page->is_dirty = true;
        dPage_entry->page->need_flushing = false;
    }
    return PagerResult::Success;
}

PagerResult Pager::commit_phase_two() {
    if (!is_open) return PagerResult::DatabaseNotOpen;
    assert(write_txn_state != WriteTxnState::DirtyInMemory);

    bool valid_journal = false;
    PagerResult journal_check = journal_has_contents(valid_journal);
    if (journal_check != PagerResult::Success) return PagerResult::JournalTruncateFailed;
    assert(valid_journal);
    if (!valid_journal) return PagerResult::JournalTruncateFailed;

    std::fstream jFile(jFile_name, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!jFile.is_open() || jFile.fail()) {
        return PagerResult::JournalTruncateFailed;
    }

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
    return PagerResult::Success;
}

PagerResult Pager::rollback_transaction() {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    if (write_txn_state == WriteTxnState::None) return PagerResult::Success;
    if (write_txn_state == WriteTxnState::DirtyInMemory) {
        return cleanup_transaction_cache();
    }

    return rollback_hot_journal();
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

    bool valid_journal = false;
    PagerResult journal_check = journal_has_contents(valid_journal);
    if (journal_check != PagerResult::Success) return PagerResult::JournalOpenFailed;
    if (!valid_journal) {
        if (write_txn_state == WriteTxnState::DirtyInMemory) {
            return cleanup_transaction_cache();
        }
        assert(write_txn_state != WriteTxnState::JournalDurable);
        return PagerResult::JournalOpenFailed;
    }

    std::fstream jFile(jFile_name, std::ios::in | std::ios::out | std::ios::binary);
    if (!jFile.is_open() || jFile.fail()) {
        return PagerResult::JournalOpenFailed;
    }
    std::size_t journal_file_size = 0;
    try {
        journal_file_size = disk::file_size(jFile);
    } catch (const std::exception &) {
        return PagerResult::JournalOpenFailed;
    }

    std::unordered_set<int> restored_page_nums;
    std::streamoff curr_offset = 0;
    bool stop_recovery = false;
    bool has_recovery_target_page_count = false;
    std::uint32_t recovery_target_page_count = 0;
    while (curr_offset < static_cast<std::streamoff>(journal_file_size) && !stop_recovery) {
        char jHeader_bytes[JOURNAL_HEADER_SIZE];
        try {
            disk::seek_read_to(jFile, curr_offset);
            disk::read_exact(jFile, jHeader_bytes);
        } catch (const std::exception &) {
            if (curr_offset == 0) return PagerResult::JournalOpenFailed;
            break;
        }

        JournalHeader jHeader;
        Journal::deserialize_jHeader(jHeader, jHeader_bytes);

        bool is_valid_header = Journal::validate_journal_header(jHeader);
        if (!is_valid_header) {
            if (curr_offset == 0) {
                jFile.close();
                try {
                    std::filesystem::resize_file(jFile_name, 0);
                } catch (const std::exception &) {
                    return PagerResult::JournalTruncateFailed;
                }
                return PagerResult::JournalCorrupt;
            }
            break;
        }

        if (!has_recovery_target_page_count) {
            recovery_target_page_count = jHeader.init_db_page_count;
            has_recovery_target_page_count = true;
        } else if (recovery_target_page_count != jHeader.init_db_page_count) {
            return PagerResult::JournalCorrupt;
        }

        int page_count = static_cast<int>(jHeader.page_count);
        std::streamoff records_offset = curr_offset + static_cast<std::streamoff>(PAGE_SIZE);

        for (int i = 0; i < page_count; i++) {
            char jPage_record_bytes[JOURNAL_PAGE_RECORD];
            std::streamoff j_offset = records_offset + static_cast<std::streamoff>(i) * static_cast<std::streamoff>(JOURNAL_PAGE_RECORD);
            try {
                disk::seek_read_to(jFile, j_offset);
                disk::read_exact(jFile, jPage_record_bytes);
            } catch (const std::exception &) {
                stop_recovery = true;
                break;
            }

            JournalPageRecord jPage_record;
            Journal::deserialize_jPage_record(jPage_record, jPage_record_bytes);

            bool is_valid_record = Journal::validate_journal_record_checksum(jPage_record, jHeader);
            if (!is_valid_record) {
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
                disk::seek_write_to(dbFile_handler, db_offset);
                disk::write_exact(dbFile_handler, jPage_record.data);
            } catch (const std::exception &) {
                return PagerResult::DbWriteFailed;
            }
        }

        if (stop_recovery) break;

        std::streamoff next_header_offset = align_to_page_boundary(
            records_offset + static_cast<std::streamoff>(page_count) * static_cast<std::streamoff>(JOURNAL_PAGE_RECORD)
        );
        if (next_header_offset >= static_cast<std::streamoff>(journal_file_size)) {
            break;
        }
        curr_offset = next_header_offset;
    }

    dbFile_handler.flush();
    if (dbFile_handler.bad() || dbFile_handler.fail()) {
        return PagerResult::DbFlushFailed;
    }
    try {
        disk::sync_file_to_disk(db_name);
    } catch (const std::exception &) {
        return PagerResult::DbFlushFailed;
    }

    if (has_recovery_target_page_count) {
        try {
            std::uintmax_t current_db_size = std::filesystem::file_size(db_name);
            std::uintmax_t target_db_size = static_cast<std::uintmax_t>(recovery_target_page_count) * PAGE_SIZE;
            if (current_db_size > target_db_size) {
                if (dbFile_handler.is_open()) dbFile_handler.close();
                std::filesystem::resize_file(db_name, target_db_size);
                dbFile_handler.open(db_name, std::ios::in | std::ios::out | std::ios::binary);
                if (!dbFile_handler.is_open() || dbFile_handler.fail()) {
                    return PagerResult::OpenDbFailed;
                }
            }
        } catch (const std::exception &) {
            return PagerResult::DbFlushFailed;
        }
    }

    if (jFile.is_open()) jFile.close();
    try {
        std::filesystem::resize_file(jFile_name, 0);
    } catch (const std::exception &) {
        return PagerResult::JournalTruncateFailed;
    }

    if (write_txn_state != WriteTxnState::None || !dirty_pages.empty()) {
        PagerResult cleanup_result = cleanup_transaction_cache();
        if (cleanup_result != PagerResult::Success) return cleanup_result;
    }

    return load_db_header_from_disk();
}

/** Private helpers */

PagerResult Pager::journal_has_contents(bool &has_contents) {
    has_contents = false;
    try {
        has_contents = std::filesystem::exists(jFile_name);
        if (has_contents) {
            has_contents = !std::filesystem::is_empty(jFile_name);
        }
    } catch (const std::exception &) {
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

    bool hot_journal_exists = false;
    PagerResult journal_check = journal_has_contents(hot_journal_exists);
    if (journal_check != PagerResult::Success) return journal_check;
    if (!hot_journal_exists) return PagerResult::Success;

    PagerResult recovery_result = rollback_hot_journal();
    if (recovery_result != PagerResult::Success) return recovery_result;
    return load_db_header_from_disk();
}

PagerResult Pager::cleanup_transaction_cache() {
    // This invalidates the current dirty pages in cache for recovery from a failed write transaction
    // called in rollback_hot_journal only iff the process calling rollback is the one that was doing the transaction
    // or if the dirty pages list is not empty

    for (auto it = dirty_pages.begin(); it != dirty_pages.end(); ) {
        int page_num = it->first;
        DirtyPageEntry *dPage_entry = it->second;
        Page *cached_page = pCache->get(page_num);

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
            std::memcpy(cached_page->data, dPage_entry->backup_image, PAGE_SIZE);
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

PagerResult Pager::create_new_database_file() {
    DBHeader initial_header;
    initial_header.db_page_count = 1;

    char header_page[PAGE_SIZE] = {};
    DBHeaderCodec::serialize_DBHeader(initial_header, header_page);

    std::fstream dbFile(db_name, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!dbFile.is_open() || dbFile.fail()) {
        return PagerResult::OpenDbFailed;
    }

    try {
        std::span<const char> header_page_span(header_page, PAGE_SIZE);
        disk::write_exact(dbFile, header_page_span);
    } catch (const std::exception &) {
        return PagerResult::OpenDbFailed;
    }

    dbFile.flush();
    if (dbFile.bad() || dbFile.fail()) {
        return PagerResult::OpenDbFailed;
    }
    dbFile.close();

    try {
        disk::sync_file_to_disk(db_name);
    } catch (const std::exception &) {
        return PagerResult::OpenDbFailed;
    }

    db_header = initial_header;
    return PagerResult::Success;
}

PagerResult Pager::load_db_header_from_disk() {
    char header_page[PAGE_SIZE];
    try {
        disk::seek_read_to(dbFile_handler, 0);
        std::span<char> header_page_span(header_page, PAGE_SIZE);
        disk::read_exact(dbFile_handler, header_page_span);
    } catch (const std::exception &) {
        return PagerResult::DbReadFailed;
    }

    DBHeader loaded_header;
    DBHeaderCodec::deserialize_DBHeader(loaded_header, header_page);
    if (!DBHeaderCodec::validate_DBHeader(loaded_header)) {
        return PagerResult::DbHeaderCorrupt;
    }
    if (loaded_header.db_page_count == 0) {
        return PagerResult::DbHeaderCorrupt;
    }

    std::size_t file_size = 0;
    try {
        file_size = disk::file_size(dbFile_handler);
    } catch (const std::exception &) {
        return PagerResult::DbReadFailed;
    }
    if (file_size % PAGE_SIZE != 0) return PagerResult::DbHeaderCorrupt;
    if (loaded_header.db_page_count > file_size / PAGE_SIZE) return PagerResult::DbHeaderCorrupt;

    db_header = loaded_header;

    Page *cached_header_page = pCache->get(DB_HEADER_PAGE_NUM);
    if (cached_header_page) {
        std::memcpy(cached_header_page->data, header_page, PAGE_SIZE);
        cached_header_page->is_dirty = false;
        cached_header_page->need_flushing = false;
    }

    return PagerResult::Success;
}

PagerResult Pager::ensure_header_page_loaded(Page *&page) {
    return get_or_load_page(DB_HEADER_PAGE_NUM, page);
}

PagerResult Pager::get_or_load_page(int page_num, Page *&page) {
    page = pCache->get(page_num);
    if (page) return PagerResult::Success;

    PagerReadPageResult read_result = read_page_from_disk(page_num);
    if (read_result.status != PagerResult::Success) {
        return read_result.status;
    }

    page = read_result.page;
    return cache_put(page);
}

PagerResult Pager::ensure_transaction_started() {
    if (write_txn_state != WriteTxnState::None) return PagerResult::Success;

    Page *header_page = nullptr;
    PagerResult header_load_result = ensure_header_page_loaded(header_page);
    if (header_load_result != PagerResult::Success) return header_load_result;

    txn_init_header = db_header;
    txn_init_header_valid = true;
    write_txn_state = WriteTxnState::DirtyInMemory;

    PagerResult dirty_header_result = mark_loaded_page_dirty(header_page);
    if (dirty_header_result != PagerResult::Success) return dirty_header_result;

    db_header.file_change_counter++;
    sync_db_header_to_cached_header_page();
    return PagerResult::Success;
}

PagerResult Pager::mark_header_dirty_for_mutation() {
    PagerResult txn_result = ensure_transaction_started();
    if (txn_result != PagerResult::Success) return txn_result;

    Page *header_page = nullptr;
    PagerResult header_load_result = ensure_header_page_loaded(header_page);
    if (header_load_result != PagerResult::Success) return header_load_result;

    return mark_loaded_page_dirty(header_page);
}

PagerResult Pager::mark_loaded_page_dirty(Page *page) {
    if (page->is_dirty && page->need_flushing) return PagerResult::Success;

    if (page->is_dirty && !page->need_flushing) {
        assert(write_txn_state != WriteTxnState::None);
        page->need_flushing = true;
        return PagerResult::Success;
    }

    page->is_dirty = true;
    page->need_flushing = true;
    auto it = dirty_pages.find(page->page_num);
    if (it == dirty_pages.end()) {
        DirtyPageEntry *new_entry = new DirtyPageEntry();
        new_entry->backup_image = new char[PAGE_SIZE];
        std::memcpy(new_entry->backup_image, page->data, PAGE_SIZE);
        new_entry->page = page;
        dirty_pages[page->page_num] = new_entry;
    } else {
        it->second->page = page;
    }
    return PagerResult::Success;
}

void Pager::sync_db_header_to_cached_header_page() {
    Page *header_page = pCache->get(DB_HEADER_PAGE_NUM);
    assert(header_page != nullptr);
    DBHeaderCodec::serialize_DBHeader(db_header, header_page->data);
}

void Pager::read_freelist_links(Page *page, std::uint32_t &next_page_num, std::uint32_t &prev_page_num) {
    next_page_num = get_u32_be(&page->data[FREELIST_NEXT_OFFSET]);
    prev_page_num = get_u32_be(&page->data[FREELIST_PREV_OFFSET]);
}

void Pager::write_freelist_links(Page *page, std::uint32_t next_page_num, std::uint32_t prev_page_num) {
    put_u32_be(&page->data[FREELIST_NEXT_OFFSET], next_page_num);
    put_u32_be(&page->data[FREELIST_PREV_OFFSET], prev_page_num);
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
    Page *page = nullptr;
    try {
        disk::seek_read_to(dbFile_handler, offset);

        page = make_page(page_num);

        std::span<char> buffer_span(page->data);
        disk::read_exact(dbFile_handler, buffer_span);
    } catch (const std::exception &) {
        delete page;
        result.status = PagerResult::DbReadFailed;
        return result;
    }
    result.page = page;
    return result;
}

void Pager::handle_cache_eviction(const PCacheEviction &eviction) {
    if (!eviction.happened || !eviction.was_dirty) return;

    auto it = dirty_pages.find(eviction.page_num);
    if (it == dirty_pages.end()) return;

    destroy_dirty_page_entry(it->second);
    dirty_pages.erase(it);
}

PagerResult Pager::cache_put(Page *page) {
    PCachePutResult put_result = pCache->put(page);
    if (put_result.status == PCacheResult::Success) {
        handle_cache_eviction(put_result.eviction);
        return PagerResult::Success;
    }

    if (put_result.status == PCacheResult::DirtyFlush) {
        PagerResult phase_one_result = commit_phase_one();
        if (phase_one_result != PagerResult::Success) {
            delete page;
            return phase_one_result;
        }

        put_result = pCache->put(page);
        if (put_result.status == PCacheResult::Success) {
            handle_cache_eviction(put_result.eviction);
            return PagerResult::Success;
        }
    }

    delete page;

    if (put_result.status == PCacheResult::NoVictim) {
        return PagerResult::NoEvictablePage;
    }
    if (put_result.status == PCacheResult::DirtyFlush) {
        return PagerResult::CacheSpillFailed;
    }

    return PagerResult::CacheSpillFailed;
}
