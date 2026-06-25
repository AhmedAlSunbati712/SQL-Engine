#pragma once

#include <cstddef>
#include <cstdint>
#include <ios>
#include <Endian.h>

#define PAGE_SIZE 4096
constexpr std::size_t DB_HEADER_SIZE = 32;

constexpr std::streamoff align_to_page_boundary(std::streamoff offset) {
    return (offset + PAGE_SIZE - 1) & ~(static_cast<std::streamoff>(PAGE_SIZE) - 1);
}
static const char DBHeader_magic_string[16] = {0x53, 0x51, 0x4c, 0x69, 0x74, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x20, 0x33, 0x00};

struct Page {
    char data[4096];
    int page_num;
    int refs_num;
    bool is_dirty;
    bool need_flushing;
};

struct DirtyPageEntry {
            char *backup_image = nullptr;
            Page *page = nullptr;
};


struct DBHeader {
    char magic_header[16] = {0x53, 0x51, 0x4c, 0x69, 0x74, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x20, 0x33, 0x00};
    uint32_t file_change_counter = 0;
    uint32_t db_page_count = 0;
    uint32_t freelist_head_page_num = 0;
    uint32_t freelist_page_count = 0;
};
