#pragma once

#include <cstdint>
#define PAGE_SIZE 4096

struct Page {
    char data[4096];
    int page_num;
    int refs_num;
    bool is_dirty;
};

struct JournalHeader {
    char magic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
    std::uint32_t page_count;
    std::uint32_t nonce;
    std::uint32_t init_db_page_count;
    
}__attribute__((packed));

struct JournalPageRecord {
    std::uint32_t page_num;
    char data[PAGE_SIZE];
    std::uint32_t checksum;
}__attribute__((packed));