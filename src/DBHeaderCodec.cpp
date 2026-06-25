#include <DBHeaderCodec.h>
#include <Endian.h>

namespace DBHeaderCodec {
    void serialize_DBHeader(DBHeader &db_header, char *out) {
        // Magic header string bytes
        for (int idx = 0; idx < 16; idx++) {
            out[idx] = db_header.magic_header[idx];
        }
        put_u32_be(&out[16], db_header.file_change_counter);
        put_u32_be(&out[16 + 4], db_header.db_page_count);
        put_u32_be(&out[16 + 2 * 4], db_header.freelist_head_page_num);
        put_u32_be(&out[16 + 3 * 4], db_header.freelist_page_count);
    }

    void deserialize_DBHeader(DBHeader &db_header, char *in) {
        for (int idx = 0; idx < 16; idx++) {
            db_header.magic_header[idx] = in[idx];
        }
        db_header.file_change_counter = get_u32_be(&in[16]);
        db_header.db_page_count = get_u32_be(&in[16 + 4]);
        db_header.freelist_head_page_num = get_u32_be(&in[16 + 2 * 4]);
        db_header.freelist_page_count = get_u32_be(&in[16 + 3 * 4]);
    }

    bool validate_DBHeader(DBHeader &db_header) {
        for (int idx = 0; idx < 16; idx++) {
            if (db_header.magic_header[idx] != DBHeader_magic_string[idx]) return false;
        }

        if (db_header.freelist_page_count == 0) {
            return db_header.freelist_head_page_num == 0;
        }

        if (db_header.freelist_head_page_num == 0) return false;
        if (db_header.freelist_head_page_num > db_header.db_page_count) return false;

        return true;
    }
}
