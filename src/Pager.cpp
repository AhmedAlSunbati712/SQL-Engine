#include <Pager.h>

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
        // We need too check if the cache has enough space for a read
        // Which means either pCache->length < capacity or there's a free
        // unpinned page (not dirty). Instead of handling this here, it should be handled in 
        // the cache. Would be nice tho if there's a fast way to check if there's
        // an unpinned page that is not dirty. For the early versions, we are going to throw an error
        // when the only available pages are dirty so it's important in this case that we dont even 
        // waste time trying to fetch from disk the page. but later on, the cache itself will handle
        // flushing the dirty unpinned page so doesn't matter.

        // TODO: Read from disk and save in cache
    }
    page->refs_num += 1;
    if (page->refs_num == 1) pCache->pin_page(page_num);
    return page->data;
}

bool Pager::write(int page_num) {
    /**
     * Read the page first through Pager::get. This will check if the page exists
     * in cache. if it doesn't, it will handle 
     * 
     * 
     * 
     * 
     */
}