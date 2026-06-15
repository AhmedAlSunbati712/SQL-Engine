#include <JournalCodec.h>
#include <random>


namespace Journal {
    void serialize_jHeader(JournalHeader &jHeader, char *out) {
        for (int i = 0; i < 8; i++) {
            out[i] = jHeader.magic[i];
        }
        put_u32_be(&out[8], jHeader.page_count);
        put_u32_be(&out[12], jHeader.nonce);
        put_u32_be(&out[16], jHeader.init_db_page_count);
        return;
    }

    void deserialize_jHeader(JournalHeader &jHeader, char *in) {
        const unsigned char expected_magic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
        for (int i = 0; i < 8; i++) {
            jHeader.magic[i] = static_cast<unsigned char>(in[i]);
        }
        jHeader.page_count = get_u32_be(&in[8]);
        jHeader.nonce = get_u32_be(&in[12]);
        jHeader.init_db_page_count = get_u32_be(&in[16]);
        return;
    }

    void serialize_jPage_record(JournalPageRecord &jPage_record, char *out) {
        put_u32_be(out, jPage_record.page_num);
        for (int i = 0; i < PAGE_SIZE; i++) {
            out[4 + i] = jPage_record.data[i];
        }
        put_u32_be(&out[4 + PAGE_SIZE], jPage_record.checksum);
    };

    void deserialize_jPage_record(JournalPageRecord &jPage_record, char *in) {
        jPage_record.page_num = get_u32_be(in);
        for (int i = 0; i < PAGE_SIZE; i++) {
            jPage_record.data[i] = in[i + 4];
        }
        jPage_record.checksum = get_u32_be(&in[4 + PAGE_SIZE]);

    };

    std::uint32_t generate_nonce() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution dist;
        return dist(gen);
    }

    std::uint32_t checksum(uint32_t nonce, std::span<const char> page) {
        uint32_t checksum_value = nonce;
        int stride = 200;
        int X = page.size() - stride;
        while (X >= 0) {
            checksum_value += static_cast<uint32_t>(static_cast<uint8_t>(page[X]));
            X -= stride;
        }
        return checksum_value;
    }

    bool validate_journal_record_checksum(JournalPageRecord &jPage_record, JournalHeader &jHeader) {
        std::uint32_t nonce = jHeader.nonce;
        std::uint32_t checksum_value = checksum(nonce, jPage_record.data);
        return checksum_value == jPage_record.checksum;
    }

    bool validate_journal_header(JournalHeader &jHeader) {
        // TODO: is it possible to define this as a global in the namespace?
        const unsigned char expected_magic[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
        for (int i = 0; i < 8; i++) {
            if (jHeader.magic[i] != expected_magic[i]) return false;
        }
        return true;
    }

}