#include <BTreePage.h>
#include <Endian.h>


void BInternalPage::decode() {
    /**
     * Structure of an internal page
     *  <----------------------------------------------------------------                  4KB                  ---------------------------------------------------------------->
     *  ___________.__________._____________________________.___________________________.__________________________.___________________.___________.___________________________.___________.
     * |           |          |                             |                           |                          |                   |           |                           |           |
     * | 1 Byte    | 2 Bytes  |           2 Bytes           |        2 Bytes -->        |         4 Bytes          |       ....        |    ....   | Key | Right Child PageNum |    ....   |
     * |___________|__________|_____________________________|___________________________|__________________________|___________________|___________|___________________________|___________| PAGE END
     * |           |          |                             |                           |                          |                   |           |                           |           |
     * | Page Type | KeyCount | Free Region Start Offset    | Free Region End Offset    | Leftmost Child Page Num  | Cell Dir Region   |    ....   |     First Internal Cell   |    ....   |
     * |___________|__________|_____________________________|___________________________|__________________________|___________________|___________|___________________________|___________|
     *
     * KeyCount is the number of separator keys currently stored in the page.
     *
     * The cell directory region contains KeyCount 2-byte integers. Each integer stores the
     * offset from the start of the page to the start of an internal cell. The directory entries
     * are kept in sorted key order.
     *
     * Internal Cell layout
     *  __________.______________________.
     * |          |                      |
     * | 8 Bytes  |       4 Bytes        |
     * |__________|______________________|
     * |          |                      |
     * |   Key    | Right Child Page Num |
     * |__________|______________________|
     *
     * Internal pages hold M separator keys and M + 1 child page numbers.
     *
     * The leftmost child page number is stored in the page header. Then each internal cell stores
     * a separator key and the child page number to the right of that separator.
     *
     * All fixed-width numbers are serialized in big-endian format.
     */
    // First parse the fixed-size page header.
    std::uint8_t page_type_int = get_u8_be(page);
    if (page_type_int > 1) std::abort(); // TODO: REPLACE WITH SOMETHING ELSE LATER
    page_type = static_cast<PageType>(page_type_int);
    if (page_type != PageType::Internal) std::abort(); // TODO: REPLACE WITH SOMETHING ELSE LATER
    key_count = get_u16_be(&page[1]);
    free_offset_start = get_u16_be(&page[3]);
    free_offset_end = get_u16_be(&page[5]);

    // We are re-decoding this page into vectors, so drop any older decoded state first.
    keys.clear();
    child_page_nums.clear();
    keys.reserve(key_count);
    child_page_nums.reserve(static_cast<std::size_t>(key_count) + 1);

    // The first child lives in the header. every other child lives inside an internal cell.
    child_page_nums.push_back(get_u32_be(&page[7]));

    // The cell directory starts immediately after the 11-byte header.
    // Each entry is a 2-byte offset to the start of one internal cell.
    std::uint16_t start_ptr = 11;
    for (std::uint16_t i = 0; i < key_count; i++) {
        std::uint64_t key = 0;
        std::uint32_t right_child_page_num = 0;
        std::uint16_t cell_start_offset = get_u16_be(&page[start_ptr]);

        // Follow the directory entry to the actual cell body, then decode the key and
        // the child page number to the right of that separator.
        parse_internal_cell(&page[cell_start_offset], &key, &right_child_page_num);
        keys.push_back(key);
        child_page_nums.push_back(right_child_page_num);

        // Advance to the next 2-byte directory entry.
        start_ptr += 2;
    }
}

void BInternalPage::parse_internal_cell(const char *in, std::uint64_t *key, std::uint32_t *right_child_page_num) {
    // Internal cell format: 8-byte separator key, then 4-byte right child page number.
    *key = get_u64_be(in);
    *right_child_page_num = get_u32_be(&in[8]);
}


void BLeafPage::decode() {
    /**
     * Structure of a leaf page
     *  <---------------------------------------------------                     4KB                     --------------------------------------------------->
     *  ___________.__________._____________________________.___________________________.___________________.___________.____________________________.___________.
     * |           |          |                             |                           |                   |           |                            |           |
     * | 1 Byte    |  2 Bytes |           2 Bytes           |        2 Bytes -->        |      ....         |    ....   |  Key | Type | Size | Value |    ....   |
     * |___________|__________|_____________________________|___________________________|___________________|___________|____________________________|___________| PAGE END
     * |           |          |                             |                           |                   |           |                            |           |
     * | Page Type | KeyCount | Free Region Start Offset    | Free Region End Offset    | Cell Dir Region   |    ....   |    First Key-Value Entry   |    ....   |
     * |___________|__________|_____________________________|___________________________|___________________|___________|____________________________|___________|
     *
     * KeyCount is the number of live key-value cells currently stored in the page.
     *
     * The cell directory region contains KeyCount 2-byte integers. Each integer stores the
     * offset from the start of the page to the start of a key-value cell. The directory entries
     * are kept in sorted key order.
     * 
     * Key-Value Entry layout
     *  __________.___________.____________________.__________________________.
     * |          |           |                    |                          |
     * | 8 Bytes  | 1 Byte    |     2   Bytes      |          Variable        | 
     * |__________|___________|____________________|__________________________|
     * |          |           |                    |                          |
     * |   Key    | ValueType |     Value Size     |           Value          |
     * |__________|___________|____________________|__________________________|
     * 
     * Value Size is stored as a 2-byte integer.
     *
     * If the ValueType is an integer type, the Value payload itself may be VarInt-encoded.
     *
     * All fixed-width numbers are serialized in big-endian format.
     */
    

}
