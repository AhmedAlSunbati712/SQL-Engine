#include <Pager.h>
#include <DiskIO.h>
#include <sstream>
#include <random>
#include <filesystem>
#include <iostream>
#include <JournalCodec.h>
#include <cassert>
#include <unordered_set>

Pager::Pager() {
    pCache = new PCache();
}

Pager::~Pager() {
    for (auto it = dirty_pages.begin(); it != dirty_pages.end(); ) {
        delete it->second;
        it = dirty_pages.erase(it);
    }

    if (dbFile_handler.is_open()) dbFile_handler.close();
    delete pCache;
}

PagerResult Pager::open(std::string db_file) {
    assert(!is_open);
    if (is_open) return PagerResult::OpenDbFailed;

    dbFile_handler.open(db_file, std::ios::in | std::ios::out | std::ios::binary);
    if (!dbFile_handler.is_open() || dbFile_handler.fail()) {
        return PagerResult::OpenDbFailed;
    }

    db_name = db_file;
    jFile_name = db_file + "_journal";
    is_open = true;
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

    // Check if cache holds the page
    Page *page = pCache->get(page_num);
    
    // If it doesn't, read from disk
    if (!page) {
        // Read from disk
        PagerReadPageResult read_result = read_page_from_disk(page_num);
        if (read_result.status != PagerResult::Success) {
            result.status = read_result.status;
            return result;
        }

        page = read_result.page;
        // Add to cache
        PagerResult cache_result = cache_put(page);
        if (cache_result != PagerResult::Success) {
            result.status = cache_result;
            return result;
        }
    }
    page->refs_num += 1;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    result.data = page->data;
    return result;
}

PagerResult Pager::begin_write(int page_num) {
    /**
    * V1 for begin write: read page through get, ignore lock permissions, mark it as dirty
    * add to dirty pages disk and add to journal.
    */
    if (!is_open) return PagerResult::DatabaseNotOpen;

    Page *page = pCache->get(page_num);
    if (!page) {
        PagerReadPageResult read_result = read_page_from_disk(page_num);
        if (read_result.status != PagerResult::Success) return read_result.status;

        page = read_result.page;
        PagerResult cache_result = cache_put(page);
        if (cache_result != PagerResult::Success) return cache_result;
    }

    // If page is already dirty and still needs flushing, just return
    if (page->is_dirty && page->need_flushing) return PagerResult::Success;

    // If page is already dirty but no longer needs flushing, mark it dirty for a new spill
    if (page->is_dirty && !page->need_flushing) {
        page->need_flushing = true;
        return PagerResult::Success;
    }

    // Mark page as dirty, save a copy of its image and add it to our dirty pages map.
    page->is_dirty = true;
    page->need_flushing = true;
    auto it = dirty_pages.find(page_num);
    if (it == dirty_pages.end()) {
        DirtyPageEntry *new_entry = new DirtyPageEntry();
        std::memcpy(new_entry->backup_image, page->data, PAGE_SIZE);
        new_entry->page = page;
        dirty_pages[page_num] = new_entry;
    } else {
        it->second->page = page;
    }
    return PagerResult::Success;
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

    // First of all, check if there are any dirty pages to begin with. If there isn't, just return
    if (dirty_pages.size() == 0) {
        return PagerResult::Success;
    }
    // Check if the journal file exists
    bool journal_exists = false;
    try {
        journal_exists = std::filesystem::exists(jFile_name);
        if (journal_exists) {
            journal_exists = !std::filesystem::is_empty(jFile_name);
        }
    } catch (const std::exception &) {
        return PagerResult::JournalOpenFailed;
    }
    JournalHeader jHeader;
    
    std::fstream jFile;
    if (!journal_exists) {
        // If the file doesn't exist, open it with trunc mode so it gets created.
        jFile.open(jFile_name, std::ios::in | std::ios::out | std::ios::trunc | std::ios::binary);
        if (!jFile.is_open() || jFile.fail()) {
            // Can't open a journal file. The caller can either try again or choose to abort the transaction
            // If they abort the transaction, they need to invalidate the cache.
            // No recovery or fallback needs to be done here
            // The caller if they decide to abort the transaction might call rollback handler
            // to invalidate the cache.
            // Return code: FAIL_CREATE_JOURNAL constant
            return PagerResult::JournalCreateFailed;
        }

    } else {
        // This will only happen the case of cache spill. A transaction is trying to modify so many
        // pages that it had to at somepoint call commit_phase_one to empty space to read
        // more pages to write

        // open without the truncate flag, read the header, deserialize it and start at the end of the file
        jFile.open(jFile_name, std::ios::in | std::ios::out | std::ios::binary);

        // Caller can either try again or abort the transaction and rollback
        if (!jFile.is_open() || jFile.fail()) {
            return PagerResult::JournalOpenFailed;
        }
        // Read the header bytes
        char header_bytes[JOURNAL_HEADER_SIZE];
        try {
            disk::seek_read_to(jFile, 0);
            disk::read_exact(jFile, header_bytes);
        } catch (const std::exception &) {
            return PagerResult::JournalOpenFailed;
        }

        // Deserialize
        Journal::deserialize_jHeader(jHeader, header_bytes);

        // Check if the journal header is not corrupted (has a valid magic byte pattern)
        bool valid_journal = Journal::validate_journal_header(jHeader);
        if (!valid_journal) {
            // This is the kind of corruption we are not accounting for.
            // The database has pages that don't belong to the pre-transaction state
            // There's no way to rollback. Cooked
            return PagerResult::JournalCorrupt;
        }
        jFile.seekp(0, std::ios::end); // Seek to the end of the file (after the most recent backup image appended)
    }

    // A header should start on a page boundary so seek the smallest next page boundary
    // or stay on the current one if it's already a page boundary
    std::streamoff curr_offset = 0;
    try {
        curr_offset = disk::get_curr_write_offset(jFile);
    } catch (const std::exception &) {
        return PagerResult::JournalHeaderWriteFailed;
    }
    curr_offset = (curr_offset + PAGE_SIZE - 1) & ~(PAGE_SIZE  - 1);

    // Create the new header
    jHeader.nonce = Journal::generate_nonce();
    jHeader.init_db_page_count = 0; // TODO: Change this when we are tracking db page count
    jHeader.page_count = 0;

    // Write it to disk
    char jHeader_bytes[JOURNAL_HEADER_SIZE];
    try {
        disk::seek_write_to(jFile, curr_offset);
        Journal::serialize_jHeader(jHeader, jHeader_bytes);
        disk::write_exact(jFile, jHeader_bytes);
    } catch (const std::exception &) {
        return PagerResult::JournalHeaderWriteFailed;
    }

    // The header should have its own page/sector, so seek to the next page boundary to start writing  the records
    try {
        disk::seek_write_to(jFile, curr_offset + static_cast<std::streamoff>(PAGE_SIZE));
    } catch (const std::exception &) {
        return PagerResult::JournalRecordWriteFailed;
    }

    // Iterate through the dirty pages, generate a journal record out of it, serialize and write to disk.
    for (const auto& [page_num, dirty_page] : dirty_pages) {
        if (dirty_page->page->need_flushing) {
            JournalPageRecord jPage_record;
            jPage_record.page_num = page_num;
            for (int i = 0; i < PAGE_SIZE; i++) {
                jPage_record.data[i] = dirty_page->backup_image[i];
            }
            jPage_record.checksum = Journal::checksum(jHeader.nonce, jPage_record.data);
            jHeader.page_count++;

            // Write the journal record to disk and then seek to the end of this chunk.
            char jPage_record_bytes[JOURNAL_PAGE_RECORD];
            Journal::serialize_jPage_record(jPage_record, jPage_record_bytes);

            try {
                disk::write_exact(jFile, jPage_record_bytes);
            } catch (const std::exception &) {
                return PagerResult::JournalRecordWriteFailed;
            }

        }
    }
    // Flush and sync file to disk
    jFile.flush();
    if (jFile.bad() || jFile.fail()) {
        return PagerResult::JournalFlushFailed;
    }
    try {
        disk::sync_file_to_disk(jFile_name);
    } catch (const std::exception &) {
        return PagerResult::JournalFlushFailed;
    }
    // Now write the journal header and flush again
    char jHeader_bytes_final[JOURNAL_HEADER_SIZE];
    try {
        disk::seek_write_to(jFile, curr_offset);
        Journal::serialize_jHeader(jHeader, jHeader_bytes_final);
        disk::write_exact(jFile, jHeader_bytes_final);
    } catch (const std::exception &) {
        return PagerResult::JournalHeaderWriteFailed;
    }

    // Flush again and sync to disk
    jFile.flush();
    if (jFile.bad() || jFile.fail()) {
        // need to rollback and invalidate any dirty pages
        // Return status code: FAILED_FLUSH_JOURNAL
        return PagerResult::JournalFlushFailed;
    }
    try {
        disk::sync_file_to_disk(jFile_name);
    } catch (const std::exception &) {
        return PagerResult::JournalFlushFailed;
    }

    // Now, we need to iterate through the dirty pages again, this time to write the change db pages to disk
    for (const auto &[page_num, dPage_entry] : dirty_pages) {
        std::span<char> new_page_data(dPage_entry->page->data);
        // Seek to the correct write offset and write the data to disk
        try {
            disk::seek_write_to(dbFile_handler, static_cast<std::streamoff>(static_cast<std::streamoff>(page_num) * static_cast<std::streamoff>(PAGE_SIZE)));
            disk::write_exact(dbFile_handler, new_page_data);
        } catch (const std::exception &) {
            return PagerResult::DbWriteFailed;
        }
    }
    // Flush db pages to disk and sync
    dbFile_handler.flush();
    if (dbFile_handler.bad() || dbFile_handler.fail()) {
        return PagerResult::DbFlushFailed;
    }
    try {
        disk::sync_file_to_disk(db_name);
    } catch (const std::exception &) {
        return PagerResult::DbFlushFailed;
    }

    // Now, go through the dirty pages and mark them as not needing flushing any more
    for (auto &[page_num, dPage_entry] : dirty_pages) {
        dPage_entry->page->is_dirty = true;
        dPage_entry->page->need_flushing = false;
    }
    return PagerResult::Success;
}

PagerResult Pager::commit_phase_two() {
    if (!is_open) return PagerResult::DatabaseNotOpen;

    // Check if there doesn't exist any journal. In that case, we can't commit. Throw an error
    bool valid_journal = false;
    try {
        valid_journal = std::filesystem::exists(jFile_name) && !std::filesystem::is_empty(jFile_name);
    } catch (const std::exception &) {
        return PagerResult::JournalTruncateFailed;
    }
    assert(valid_journal);
    if (!valid_journal) return PagerResult::JournalTruncateFailed;

    // There's a journal that has content, therefore we need to truncate it.
    std::fstream jFile(jFile_name, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!jFile.is_open() || jFile.fail()) {
        return PagerResult::JournalTruncateFailed;
    }
    // On successful commit, clear the transaction-associated dirty pages that we kept around
    for (auto it = dirty_pages.begin(); it != dirty_pages.end(); ) {
        DirtyPageEntry *dPage_entry = it->second;
        if (dPage_entry->page) {
            dPage_entry->page->is_dirty = false;
            dPage_entry->page->need_flushing = false;
        }
        delete dPage_entry;
        it = dirty_pages.erase(it);
    }
    return PagerResult::Success;
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

    // Check if there's a valid journal on disk. throw if there isn't
    bool valid_journal = false;
    try {
        valid_journal = std::filesystem::exists(jFile_name) && !std::filesystem::is_empty(jFile_name);
    } catch (const std::exception &) {
        return PagerResult::JournalOpenFailed;
    }
    assert(valid_journal);
    if (!valid_journal) return PagerResult::JournalOpenFailed;

    auto align_to_page_boundary = [](std::streamoff offset) -> std::streamoff {
        return (offset + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    };

    // open the journal file and throw if we fail
    std::fstream jFile(jFile_name, std::ios::in | std::ios::out | std::ios::binary);
    if (!jFile.is_open() || jFile.fail()) {
        std::ostringstream oss;
        oss << "Error (Pager.rollback_hot_journal, Pager object: " << static_cast<void *>(this) << " ): Failed to open rollback journal";
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
    while (curr_offset < static_cast<std::streamoff>(journal_file_size) && !stop_recovery) {
        // Read a number of bytes from disk equal to the size of a header.
        char jHeader_bytes[JOURNAL_HEADER_SIZE];
        try {
            disk::seek_read_to(jFile, curr_offset);
            disk::read_exact(jFile, jHeader_bytes);
        } catch (const std::exception &) {
            if (curr_offset == 0) return PagerResult::JournalOpenFailed;
            break;
        }

        // Deserialize the bytes into an object
        JournalHeader jHeader;
        Journal::deserialize_jHeader(jHeader, jHeader_bytes);

        // If it's not a valid header, stop here and finalize recovery
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

        // If it's a valid header, get the number of pages that are in this sub-journal records
        int page_count = jHeader.page_count;
        std::streamoff records_offset = curr_offset + static_cast<std::streamoff>(PAGE_SIZE);

        // and for i <- 1 to record_num (or wtvr):
        //      read a page deserialize and validate and do all the stuff we are doing and rollback
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

            // Deserialize into a JournalPageRecord object
            JournalPageRecord jPage_record;
            Journal::deserialize_jPage_record(jPage_record, jPage_record_bytes);

            // validate checksum of the page
            bool is_valid_record = Journal::validate_journal_record_checksum(jPage_record, jHeader);
            if (!is_valid_record) {
                stop_recovery = true;
                break;
            }

            // Skip repeated pages. The first valid record we see for a page wins.
            int db_page_num = jPage_record.page_num;
            if (restored_page_nums.find(db_page_num) != restored_page_nums.end()) {
                continue;
            }
            restored_page_nums.insert(db_page_num);

            // Now seek into the write position in the db to write back this image
            std::streamoff db_offset = static_cast<std::streamoff>(db_page_num) * static_cast<std::streamoff>(PAGE_SIZE);
            try {
                disk::seek_write_to(dbFile_handler, db_offset);
                disk::write_exact(dbFile_handler, jPage_record.data);
            } catch (const std::exception &) {
                return PagerResult::DbWriteFailed;
            }
        }

        if (stop_recovery) break;

        // make sure we are at a page boundary. seek to the next smallest page boundary (or stay at the current one if its a page boundary)
        std::streamoff next_header_offset = align_to_page_boundary(records_offset + static_cast<std::streamoff>(page_count) * static_cast<std::streamoff>(JOURNAL_PAGE_RECORD));
        if (next_header_offset >= static_cast<std::streamoff>(journal_file_size)) {
            break;
        }
        curr_offset = next_header_offset;
    }
    // Need to flush and sync probably here
    dbFile_handler.flush();
    if (dbFile_handler.bad() || dbFile_handler.fail()) {
        return PagerResult::DbFlushFailed;
    }
    try {
        disk::sync_file_to_disk(db_name);
    } catch (const std::exception &) {
        return PagerResult::DbFlushFailed;
    }

    // Then eventually go over each page in dirty pages and clean up by removing it from the cache.
    for (auto it = dirty_pages.begin(); it != dirty_pages.end(); ) {
        int page_num = it->first;
        PCacheResult remove_result = pCache->remove(page_num);
        assert(remove_result != PCacheResult::RemovingPinnedPage);

        delete it->second;
        it = dirty_pages.erase(it);
    }

    if (jFile.is_open()) jFile.close();
    try {
        std::filesystem::resize_file(jFile_name, 0);
    } catch (const std::exception &) {
        return PagerResult::JournalTruncateFailed;
    }
    return PagerResult::Success;
}

/** Private helpers */

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

    // Calculate the offset. Make sure to cast to the internal type streamoff to avoid arbitrary sizes of int
    // on different platforms for offset
    std::streamoff offset = static_cast<std::streamoff>(page_num) * static_cast<std::streamoff>(PAGE_SIZE);
    Page *page = nullptr;
    try {
        disk::seek_read_to(dbFile_handler, offset);

        // Make a new page object and initialize its members
        page = new Page();
        page->page_num = page_num;
        page->refs_num = 0;
        page->is_dirty = false;
        page->need_flushing = false;

        // Make a span out of the page data array so it can be passed safely into read exact
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

    delete it->second;
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
