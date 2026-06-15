#include <Pager.h>
#include <DiskIO.h>
#include <sstream>
#include <random>
#include <filesystem>
#include <iostream>
#include <JournalCodec.h>


Pager::Pager(std::string db_file): db_name(db_file), dbFile_handler(db_file, std::ios::in | std::ios::out | std::ios::binary) {
    if (!dbFile_handler.is_open() || dbFile_handler.fail()) {
        throw std::runtime_error("Error: Failed to open database file");
    }
    jFile_name = db_file + "_journal";
    pCache = new PCache();
};

char *Pager::get(int page_num) {
    /**
     * Check in the cache. if it returns the page. return it
     * if it's not, read from disk, and put in the cache
     * increase refs
     * pin in the cache
     */

    // Check if cache holds the page
    Page *page = pCache->get(page_num);
    
    // If it doesn't, read from disk
    if (!page) {
        // Read from disk
        page = read_page_from_disk(page_num);
        // Add to cache
        cache_put_or_throw(page);
    }
    page->refs_num += 1;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    return page->data;
}

bool Pager::begin_write(int page_num) {
    /**
    * V1 for begin write: read page through get, ignore lock permissions, mark it as dirty
    * add to dirty pages disk and add to journal.
    */
    Page *page = pCache->get(page_num);
    if (!page) {
        page = read_page_from_disk(page_num);
        cache_put_or_throw(page);
    }

    // If page is already dirty, just return true
    if (page->is_dirty) return true;

    // Mark page as dirty, save a copy of its image and add it to our dirty pages map.
    page->is_dirty = true;
    page->need_flushing = true;
    DirtyPageEntry *new_entry = new DirtyPageEntry();
    std::memcpy(new_entry->backup_image, page->data, PAGE_SIZE);
    new_entry->page = page;
    dirty_pages[page_num] = new_entry;
    return true;

}


void Pager::ref_page(int page_num) {
    Page *page = pCache->get(page_num);
    if (!page) {
        std::ostringstream oss;
        oss << "Error (Pager.ref_page, Pager object: " << static_cast<void *>(this) << "): Page " << page_num << " doesn't exist in the cache.";
        throw std::runtime_error(oss.str());
    }
    page->refs_num++;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    return;
}

void Pager::unref_page(int page_num) {
    Page *page = pCache->get(page_num);
    if (!page) {
        std::ostringstream oss;
        oss << "Error (Pager.unref_page, Pager object: " << static_cast<void *>(this) << "): Page " << page_num << " doesn't exist in the cache.";
        throw std::runtime_error(oss.str());
    }

    page->refs_num--;
    if (page->refs_num == 0) pCache->unpin_page(page_num);
    return;
}


// TODO: replace all seekp with seek_write_to
void Pager::commit_phase_one() {
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

    // First of all, check if there are any dirty pages to begin with. If there isn't, just return
    if (dirty_pages.size() == 0) {
        return;
    }
    // Check if the journal file exists
    bool journal_exists = std::filesystem::exists(jFile_name) || !std::filesystem::is_empty(jFile_name);
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
            throw std::runtime_error("Error: Failed to create a journal file");
        }

    } else {
        // This will only happen the case of cache spill. A transaction is trying to modify so many
        // pages that it had to at somepoint call commit_phase_one to empty space to read
        // more pages to write

        // open without the truncate flag, read the header, deserialize it and start at the end of the file
        jFile.open(jFile_name, std::ios::in | std::ios::out | std::ios::binary);
        // In case reading failed:
        // Return code: FAILED_TO_OPEN_JOURNAL
        // In this case, the caller can either try again or abort the transaction
        // In case of aborting the transaction, they need to do the following:
        // Invalidate dirty pages. Invalidate the previously dirty pages (how to do that? maybe need the need_to_flush flag back)
        // and rollback the journal and copy into the db
        if (!jFile.is_open() || jFile.fail()) {
            throw std::runtime_error("Error: Failed to open an already existing journal file");
        }
        // Read the header bytes
        disk::seek_read_to(jFile, 0);
        char header_bytes[JOURNAL_HEADER_SIZE];
        disk::read_exact(jFile, header_bytes);

        // Deserialize
        Journal::deserialize_jHeader(jHeader, header_bytes);

        // Check if the journal header is not corrupted (has a valid magic byte pattern)
        bool valid_journal = Journal::validate_journal_header(jHeader);
        if (!valid_journal) {
            // I dont think this should possibly happen. This means, some pages were flushed
            // to disk in the db and the backup images are stored in the journal
            // However, the journal is corrupt now so we can't rollback if the user would like
            // to abort the transaction. And, we can't proceed because the journal is corrupt
            // Assumption: The user will abort the transaction without calling a rollback. That's fine
            // Return code: CORRUPT_JOURNAL
            throw std::runtime_error("Error: Journal file is corrupted (invalid magic bytes)");
        }
        jFile.seekp(0, std::ios::end); // Seek to the end of the file (after the most recent backup image appended)
    }
    // TODO: seek to the next page boundary.
    // do a ceiling operation on the boundary to align to the least next page boundary (which could be the current one)
    // std::streamoff curr_offset = disk::get_curr_wirte(jFile);
    // curr_offset = ceil(curr_offset)
    std::streamoff curr_offset = disk::get_curr_write_offset(jFile);
    curr_offset = (curr_offset + PAGE_SIZE - 1) & ~(PAGE_SIZE  - 1);

    // Create the header either way (this is for the new patch of pages)
    jHeader.nonce = Journal::generate_nonce();
    jHeader.init_db_page_count = 0; // TODO: Change this when we are tracking db page count
    jHeader.page_count = 0;

    // Write it to disk
    disk::seek_write_to(jFile, curr_offset);
    char jHeader_bytes[JOURNAL_HEADER_SIZE];
    Journal::serialize_jHeader(jHeader, jHeader_bytes);
    disk::write_exact(jFile, jHeader_bytes);

    // The header should have its own page/sector, so seek to the next page boundary to start writing  the records
    disk::seek_write_to(jFile, curr_offset + static_cast<std::streamoff>(PAGE_SIZE));

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

            // Should wrap this step in try catch. Caller can either try to call again or abort the transaction
            // Return code: FAIL_WRITE_RECORD
            // Caller options: abort & rollback or try again
            disk::write_exact(jFile, jPage_record_bytes);
            
            // 
        }
    }
    // TODO: Future me is going to be so pissed off that we have to rewrite all of this with the POSIX file utils
    // Flush the journal pages to disk
    // Actually!! c++ introduced native handle on file streams. fsync(jFile.native_handle)
    jFile.flush();
    if (jFile.bad() || jFile.fail()) {
        // need to rollback and invalidate any dirty pages
        // Return status code: FAILED_FLUSH_JOURNAL
        throw std::runtime_error("Error: Failed to flush journal");
        return;
    }
    // Now write the journal header and flush again
    disk::seek_write_to(jFile, curr_offset);
    char jHeader_bytes[JOURNAL_HEADER_SIZE];
    Journal::serialize_jHeader(jHeader, jHeader_bytes);
    disk::write_exact(jFile, jHeader_bytes);

    // Flush again
    jFile.flush();
    if (jFile.bad() || jFile.fail()) {
        // need to rollback and invalidate any dirty pages
        // Return status code: FAILED_FLUSH_JOURNAL
        throw std::runtime_error("Error: Failed to flush journal");
        return;
    }

    // Now, we need to iterate through the dirty pages again, this time to write the change db pages to disk
    for (const auto &[page_num, dPage_entry] : dirty_pages) {
        std::span<char> new_page_data(dPage_entry->page->data);
        // TODO: Is it more efficient to seek from beginning of the file or to seek from current position?
        // Seek to the correct write offset and write the data to disk
        disk::seek_write_to(dbFile_handler, static_cast<std::streamoff>(static_cast<std::streamoff>(page_num) * static_cast<std::streamoff>(PAGE_SIZE)));
        // TODO: probably need a fallback here that if an error is thrown here, we need to rollback. A rollback will also need to invalidate the cache as it's reading
        disk::write_exact(dbFile_handler, new_page_data);
    }
    // Flush db pages to disk
    dbFile_handler.flush();

    // Now, go through the dirty pages and mark them as not dirty any more
    for (auto it = dirty_pages.begin(); it != dirty_pages.end(); ) {
        DirtyPageEntry *dPage_entry = it->second;
        dPage_entry->page->is_dirty = false;
        dPage_entry->page->need_flushing = false;

        // erase and advance
        delete dPage_entry;
        it = dirty_pages.erase(it);
    }
    return;
}

void Pager::commit_phase_two() {
    // Check if there doesn't exist any journal. In that case, we can't commit. Throw an error
    bool valid_journal = std::filesystem::exists(jFile_name) && !std::filesystem::is_empty(jFile_name);
    if (!valid_journal) {
        std::ostringstream oss;
        oss << "Error (Pager.commit_phase_two, Pager object: " << static_cast<void *>(this) << " ): Can't execute second phase of the commit! Journal doesn't exist/invalid";
        throw std::runtime_error(oss.str());
    }

    // There's a journal that has content, therefore we need to truncate it.
    std::fstream jFile(jFile_name, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!jFile.is_open() || jFile.fail()) {
        std::ostringstream oss;
        oss << "Error (Pager.commit_phase_two, Pager object: " << static_cast<void *>(this) << " ): Failed to truncate journal file";
    }
    return;
}

void Pager::rollback_hot_journal() {
    /**
     * 
     */
    // Check if there's a valid journal on disk. throw if there isn't
    bool valid_journal = std::filesystem::exists(jFile_name) && !std::filesystem::is_empty(jFile_name);
    if (!valid_journal) {
        std::ostringstream oss;
        oss << "Error (Pager.rollback_hot_journal, Pager object: " << static_cast<void *>(this) << " ): Rollback journal doesn't exist on disk/is empty!";
        throw std::runtime_error(oss.str());
    }
    // open the journal file and throw if we fail
    std::fstream jFile(jFile_name, std::ios::in | std::ios::out | std::ios::binary);
    if (!jFile.is_open() || jFile.fail()) {
        std::ostringstream oss;
        oss << "Error (Pager.rollback_hot_journal, Pager object: " << static_cast<void *>(this) << " ): Failed to open rollback journal";
        throw std::runtime_error(oss.str());
    }

    // Read the journal header from disk
    char jHeader_bytes[JOURNAL_HEADER_SIZE];
    disk::seek_read_to(jFile, 0);
    disk::read_exact(jFile, jHeader_bytes);

    // Deserialize the bytes into an object
    JournalHeader jHeader;
    Journal::deserialize_jHeader(jHeader, jHeader_bytes);

    bool is_valid_header = Journal::validate_journal_header(jHeader);
    if (!is_valid_header) {
        jFile.close();
        std::filesystem::resize_file(jFile_name, 0);

        std::ostringstream oss;
        oss << "Error (Pager.rollback_hot_journal, Pager object: " << static_cast<void *>(this) << " ): Journal header is corrupted";
        throw std::runtime_error(oss.str());
    }

    // Read number of pages that are to be recovered
    int page_count = jHeader.page_count;

    // Now read each page and recover it into the db
    for (int i = 1; i <= page_count; i++) {
        // Read record bytes
        char jPage_record_bytes[JOURNAL_PAGE_RECORD];
        std::streamoff j_offset = static_cast<std::streamoff>(i) * static_cast<std::streamoff>(PAGE_SIZE);
        disk::seek_read_to(jFile, j_offset);
        disk::read_exact(jFile, jPage_record_bytes);

        // Deserialize into a JournalPageRecord object
        JournalPageRecord jPage_record;
        Journal::deserialize_jPage_record(jPage_record, jPage_record_bytes);

        // validate checksum of the page
        bool is_valid_record = Journal::validate_journal_record_checksum(jPage_record, jHeader);
        if (!is_valid_record) {
            break;
        }

        // Now seek into the write position in the db to write back this image
        int db_page_num = jPage_record.page_num;
        std::streamoff db_offset = static_cast<std::streamoff>(db_page_num) * static_cast<std::streamoff>(PAGE_SIZE);
        disk::seek_write_to(dbFile_handler, db_offset);
        disk::write_exact(dbFile_handler, jPage_record.data);
    }
    // Need to flush and sync probably here
    dbFile_handler.flush();

    if (jFile.is_open()) jFile.close();
    std::filesystem::resize_file(jFile_name, 0);
    return;
}

/** Private helpers */

Page *Pager::read_page_from_disk(int page_num) {
    // We need too check if the cache has enough space for a read
    // Which means either pCache->length < capacity or there's a free
    // unpinned page (not dirty). Instead of handling this here, it should be handled in 
    // the cache. Would be nice tho if there's a fast way to check if there's
    // an unpinned page that is not dirty. For the early versions, we are going to throw an error
    // when the only available pages are dirty so it's important in this case that we dont even 
    // waste time trying to fetch from disk the page. but later on, the cache itself will handle
    // flushing the dirty unpinned page so doesn't matter.


    // Calculate the offset. Make sure to cast to the internal type streamoff to avoid arbitrary sizes of int
    // on different platforms for offset
    std::streamoff offset = static_cast<std::streamoff>(page_num) * static_cast<std::streamoff>(PAGE_SIZE);
    disk::seek_read_to(dbFile_handler, offset);

    // Make a new page object and initialize its members
    Page *page = new Page();
    page->page_num = page_num;
    page->refs_num = 0;
    page->is_dirty = false;
    page->need_flushing = false;

    // Make a span out of the page data array so it can be passed safely into read exact
    std::span<char> buffer_span(page->data);
    disk::read_exact(dbFile_handler, buffer_span);
    return page;
}

void Pager::handle_cache_eviction(const PCacheEviction &eviction) {
    if (!eviction.happened || !eviction.was_dirty) return;

    auto it = dirty_pages.find(eviction.page_num);
    if (it == dirty_pages.end()) return;

    delete it->second;
    dirty_pages.erase(it);
}

void Pager::cache_put_or_throw(Page *page) {
    PCachePutResult put_result = pCache->put(page);
    if (put_result.status == PCacheResult::Success) {
        handle_cache_eviction(put_result.eviction);
        return;
    }

    if (put_result.status == PCacheResult::DirtyFlush) {
        commit_phase_one();
        put_result = pCache->put(page);
        if (put_result.status == PCacheResult::Success) {
            handle_cache_eviction(put_result.eviction);
            return;
        }
    }

    delete page;

    if (put_result.status == PCacheResult::NoVictim) {
        throw std::runtime_error("Error: Failed to cache page because no evictable page exists");
    }
    if (put_result.status == PCacheResult::DirtyFlush) {
        throw std::runtime_error("Error: Failed to cache page after spill/flush retry");
    }

    throw std::runtime_error("Error: Failed to cache page");
}
