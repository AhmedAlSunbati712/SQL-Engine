#include <Pager.h>
#include <DiskIO.h>
#include <sstream>
#include <random>


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
