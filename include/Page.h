#pragma once

#include <cstdint>
#include <Endian.h>

#define PAGE_SIZE 4096

struct Page {
    char data[4096];
    int page_num;
    int refs_num;
    bool is_dirty;
    bool need_flushing;
};

struct DirtyPageEntry {
			char backup_image[PAGE_SIZE];
			Page *page;
};
