#pragma once

#define PAGE_SIZE = 4096;

struct Page {
    char data[4096];
    int page_num;
    int refs_num;
    bool is_dirty;
    bool need_to_flush_journal;
};