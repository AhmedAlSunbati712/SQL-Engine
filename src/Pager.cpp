#include <Pager.h>
#include <DiskIO.h>
#include <sstream>
#include <random>
#include <filesystem>
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
        pCache->put(page);
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
        pCache->put(page);
    }

    // If page is already dirty, just return true
    if (page->is_dirty) return true;

    // Mark page as dirty, save a copy of its image and add it to our dirty pages map.
    page->is_dirty = true;
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
    if (page->refs_num == 1) pCache->unpin_page(page_num);
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
    page->is_dirty = false;

    // Make a span out of the page data array so it can be passed safely into read exact
    std::span<char> buffer_span(page->data);
    disk::read_exact(dbFile_handler, buffer_span);
    return page;
}

// TODO: replace all seekp with seek_write_to
bool Pager::commit_phase_one() {
    /**
     * flush the dirty pages to the journal
     * flush the journal header to disk.
     * now go through the same pages again and flush them to the db file
     */

    // Check if the journal file exists
    bool journal_exists = std::filesystem::exists(jFile_name) || !std::filesystem::is_empty(jFile_name);
    JournalHeader jHeader;

    std::fstream jFile;
    if (!journal_exists) {
        // If the file doesn't exist, open it with trunc mode so it gets created.
        jFile.open(jFile_name, std::ios::in | std::ios::out | std::ios::trunc | std::ios::binary);

        // Create the header
        jHeader.nonce = Journal::generate_nonce();
        jHeader.init_db_page_count = 0; // TODO: Change this when we are tracking db page count
        jHeader.page_count = 0;

        // Write it to disk
        disk::seek_write_to(jFile, 0);
        char jHeader_bytes[JOURNAL_HEADER_SIZE];
        Journal::serialize_jHeader(jHeader, jHeader_bytes);
        disk::write_exact(jFile, jHeader_bytes);

        // Seek to the 4KB page boundary. A header occupies it's own space
        disk::seek_write_to(jFile, static_cast<std::streamoff>(PAGE_SIZE));
    } else {
        // Otherwise, just open it without the truncate flag, read the header, deserialize it and start at the end of the file
        jFile.open(jFile_name, std::ios::in | std::ios::out | std::ios::binary);

        // Read the header bytes
        disk::seek_read_to(jFile, 0);
        char header_bytes[JOURNAL_HEADER_SIZE];
        disk::read_exact(jFile, header_bytes);

        // Deserialize
        Journal::deserialize_jHeader(jHeader, header_bytes);
        jFile.seekp(0, std::ios::end);
    }

    // Iterate through the dirty pages, generate a journal record out of it, serialize and write to disk.
    for (const auto& [page_num, dirty_page] : dirty_pages) {
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
        disk::write_exact(jFile, jPage_record_bytes);
    }
    // TODO: Future me is going to be so pissed off that we have to rewrite all of this with the POSIX file utils
    // Flush the journal pages to disk
    jFile.flush();

    // Now write the journal header and flush again
    disk::seek_write_to(jFile, 0);
    char jHeader_bytes[JOURNAL_HEADER_SIZE];
    Journal::serialize_jHeader(jHeader, jHeader_bytes);
    disk::write_exact(jFile, jHeader_bytes);

    // Flush again
    jFile.flush();

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

        // erase and advance
        delete dPage_entry;
        it = dirty_pages.erase(it);
    }
    return true;

}