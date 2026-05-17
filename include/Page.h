#pragma once

struct Page {
    char data[4096];
    int page_num;
    int refs_num;
    bool is_dirty;
    bool need_to_flush_journal;
};