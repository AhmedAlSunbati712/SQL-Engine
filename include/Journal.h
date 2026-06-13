#pragma once
#include <Endian.h>
#include <stdexcept>
#include <Page.h>

#define JOURNAL_HEADER_SIZE 20
#define JOURNAL_PAGE_RECORD 4104 // 4 + 4 + PAGE_SIZE = 4096


struct JournalHeader {
    unsigned char magic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
    std::uint32_t page_count;
    std::uint32_t nonce;
    std::uint32_t init_db_page_count;
};

struct JournalPageRecord {
    std::uint32_t page_num;
    char data[PAGE_SIZE];
    std::uint32_t checksum;
};