#pragma once

#include <cstdint>
#include <Endian.h>

#define PAGE_SIZE 4096

struct Page {
    char data[4096];
    int page_num;
    int refs_num;
    bool is_dirty;
};

struct JournalHeader {
    static void serialize(JournalHeader *jHeader, char *out) {
        for (int i = 0; i < 8; i++) {
            out[i] = jHeader->magic[i];
        }
        put_u32_be(&out[8], jHeader->page_count);
        put_u32_be(&out[12], jHeader->nonce);
        put_u32_be(&out[16], jHeader->init_db_page_count);
        return;
    }

    static void deserialize(JournalHeader *jHeader, const char *in) {
        const unsigned char expected_magic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
        for (int i = 0; i < 8; i++) {
            if (static_cast<unsigned char>(in[i]) != expected_magic[i]) {
                 throw std::runtime_error("JournalHeader::deserialize: invalid magic bytes");
            }
            jHeader->magic[i] = static_cast<unsigned char>(in[i]);
        }
        jHeader->page_count = get_u32_be(&in[8]);
        jHeader->nonce = get_u32_be(&in[12]);
        jHeader->init_db_page_count = get_u32_be(&in[16]);
        return;
    };
    unsigned char magic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
    std::uint32_t page_count;
    std::uint32_t nonce;
    std::uint32_t init_db_page_count;
    
};

struct JournalPageRecord {
    std::uint32_t page_num;
    char data[PAGE_SIZE];
    std::uint32_t checksum;
}__attribute__((packed));