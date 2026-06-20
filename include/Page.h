#pragma once

#include <cstdint>
#include <ios>
#include <Endian.h>

#define PAGE_SIZE 4096

constexpr std::streamoff align_to_page_boundary(std::streamoff offset) {
    return (offset + PAGE_SIZE - 1) & ~(static_cast<std::streamoff>(PAGE_SIZE) - 1);
}

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
