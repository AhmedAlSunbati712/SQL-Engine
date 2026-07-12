#include <BTreePage.h>
#include <Endian.h>
#include <Value.h>
#include <cstdlib>
#include <cstring>

BTreePage::BTreePage(char *page) : page(page) {}

PageType BTreePage::peek_page_type(const char *page) {
    std::uint8_t page_type_int = get_u8_be(page);
    if (page_type_int > 1) std::abort(); // TODO: REPLACE WITH SOMETHING ELSE LATER
    return static_cast<PageType>(page_type_int);
}

bool BTreePage::is_leaf() const {
    return page_type == PageType::Leaf;
}

std::uint16_t BTreePage::get_key_count() {
    return key_count;
}
std::size_t BTreePage::lower_bound_key(std::uint64_t key) const {
    std::size_t left = 0;
    std::size_t right = keys.size();

    while (left < right) {
        std::size_t mid = left + (right - left) / 2;
        if (keys[mid] < key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return left;
}

std::optional<std::uint64_t> BTreePage::first_key() const {
    if (keys.empty()) return std::nullopt;
    return keys[0];
}

std::optional<std::uint64_t> BTreePage::key_at(std::size_t idx) const {
    if (idx >= keys.size()) return std::nullopt;
    return keys[idx];
}

void BTreePage::write_back() {
    flush();
}

// ========================================= Internal Pages ======================================

BInternalPage::BInternalPage(char *page) : BTreePage(page) {
    decode();
}

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

void BInternalPage::write_back() {
    flush();
}

bool BInternalPage::insert_separator_at(std::size_t idx, std::uint64_t key, std::uint32_t right_child_page_num) {
    if (idx > keys.size()) return false;
    if (child_page_nums.size() != keys.size() + 1) return false;

    keys.insert(keys.begin() + idx, key);
    child_page_nums.insert(child_page_nums.begin() + idx + 1, right_child_page_num);
    key_count = static_cast<std::uint16_t>(keys.size());
    return true;
}

bool BInternalPage::remove_separator_at(std::size_t idx) {
    if (idx >= keys.size()) return false;
    if (child_page_nums.size() != keys.size() + 1) return false;

    keys.erase(keys.begin() + idx);
    child_page_nums.erase(child_page_nums.begin() + idx + 1);
    key_count = static_cast<std::uint16_t>(keys.size());
    return true;
}

bool BInternalPage::set_separator_key_at(std::size_t idx, std::uint64_t key) {
    if (idx >= keys.size()) return false;
    keys[idx] = key;
    return true;
}

bool BInternalPage::set_leftmost_child(std::uint32_t leftmost_child_page_num) {
    if (!child_page_nums.empty()) return false;
    child_page_nums.push_back(leftmost_child_page_num);
    return true;
}

bool BInternalPage::replace_leftmost_child(std::uint32_t leftmost_child_page_num) {
    if (child_page_nums.empty()) return false;
    child_page_nums[0] = leftmost_child_page_num;
    return true;
}

std::optional<std::uint32_t> BInternalPage::get_leftmost_child() const {
    if (child_page_nums.empty()) return std::nullopt;
    return child_page_nums[0];
}

std::optional<std::uint32_t> BInternalPage::get_left_child(std::size_t separator_idx) const {
    if (separator_idx >= keys.size()) return std::nullopt;
    if (child_page_nums.size() != keys.size() + 1) return std::nullopt;
    return child_page_nums[separator_idx];
}

std::optional<std::uint32_t> BInternalPage::get_right_child(std::size_t separator_idx) const {
    if (separator_idx >= keys.size()) return std::nullopt;
    if (child_page_nums.size() != keys.size() + 1) return std::nullopt;
    return child_page_nums[separator_idx + 1];
}

bool BInternalPage::remove(std::uint64_t key) {
    std::size_t idx = lower_bound_key(key);
    if (idx >= keys.size()) return false;
    if (keys[idx] != key) return false;
    return remove_separator_at(idx);
}

void BInternalPage::flush() {
    // Clear the page first so any stale bytes from older layouts do not survive after flush.
    std::memset(page, 0, 4096);

    // Serialize the fixed-size header first.
    put_u8_be(page, static_cast<std::uint8_t>(page_type));
    put_u16_be(&page[1], static_cast<std::uint16_t>(keys.size()));
    put_u16_be(&page[3], static_cast<std::uint16_t>(11 + keys.size() * 2));
    put_u16_be(&page[5], static_cast<std::uint16_t>(4096 - keys.size() * 12 - 1));
    put_u32_be(&page[7], child_page_nums[0]);

    // The directory is stored in key order. Each 2-byte entry points at the start of one
    // internal cell body somewhere in the packed cell-content region at the end of the page.
    std::uint16_t dir_ptr = 11;
    std::uint16_t cell_ptr = 4096;

    for (std::uint16_t i = 0; i < keys.size(); i++) {
        cell_ptr -= 12;

        // Internal cell format: 8-byte separator key, then 4-byte right child page number.
        put_u64_be(&page[cell_ptr], keys[i]);
        put_u32_be(&page[cell_ptr + 8], child_page_nums[i + 1]);

        // Store the offset to this cell in the directory entry for the same logical key index.
        put_u16_be(&page[dir_ptr], cell_ptr);
        dir_ptr += 2;
    }
}

void BInternalPage::parse_internal_cell(const char *in, std::uint64_t *key, std::uint32_t *right_child_page_num) {
    // Internal cell format: 8-byte separator key, then 4-byte right child page number.
    *key = get_u64_be(in);
    *right_child_page_num = get_u32_be(&in[8]);
}

void BInternalPage::fill_initial_layout(char *out) {
    std::memset(out, 0, 4096);
    put_u8_be(out, static_cast<std::uint8_t>(PageType::Internal));
    put_u16_be(&out[1], 0);
    put_u16_be(&out[3], 11);
    put_u16_be(&out[5], 4095);
    put_u32_be(&out[7], 0);
}



// ====================================== Leaf Page =======================================

BLeafPage::BLeafPage(char *page) : BTreePage(page) {
    decode();
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
    // First parse the fixed-size page header and validate that this is a leaf page.
    std::uint8_t page_type_int = get_u8_be(page);
    if (page_type_int > 1) std::abort(); // TODO: replace with something appropriate

    page_type = static_cast<PageType>(page_type_int);
    if (page_type != PageType::Leaf) std::abort(); // TODO: replace with something appropriate

    // Parse page-accounting information: key count and free-region boundaries.
    key_count = get_u16_be(&page[1]);
    free_offset_start = get_u16_be(&page[3]);
    free_offset_end = get_u16_be(&page[5]);


    // We are re-decoding this page into vectors, so drop any older decoded state first.
    keys.clear();
    values.clear();
    keys.reserve(key_count);
    values.reserve(static_cast<std::size_t>(key_count));

    // The cell directory starts immediately after the 7-byte header.
    // Each entry is a 2-byte offset to the start of one leaf cell.
    std::uint16_t start_ptr = 7;
    for (std::uint16_t i = 0; i < key_count; i++) {
        std::uint64_t key = 0;
        Value value{};
        std::uint16_t cell_start_offset = get_u16_be(&page[start_ptr]);

        // Follow the directory entry to the actual cell body, then decode the key and the value.
        parse_leaf_cell(&page[cell_start_offset], &key, &value);
        keys.push_back(key);
        values.push_back(value);

        // Advance to the next 2-byte directory entry.
        start_ptr += 2;
    }
}

void BLeafPage::write_back() {
    flush();
    return;
}

std::optional<Value> BLeafPage::get_at(std::size_t idx) const {
    if (idx >= values.size()) return std::nullopt;
    if (values.size() != keys.size()) return std::nullopt;
    return values[idx];
}

std::optional<Value> BLeafPage::get(std::uint64_t key) const {
    std::size_t idx = lower_bound_key(key);
    if (idx >= keys.size()) return std::nullopt;
    if (keys[idx] != key) return std::nullopt;
    return values[idx];
}

bool BLeafPage::insert_at(std::size_t idx, std::uint64_t key, const Value &value) {
    if (idx > keys.size()) return false;

    keys.insert(keys.begin() + idx, key);
    values.insert(values.begin() + idx, value);
    key_count = static_cast<std::uint16_t>(keys.size());
    return true;
}

bool BLeafPage::remove_at(std::size_t idx) {
    if (idx >= keys.size()) return false;
    if (values.size() != keys.size()) return false;

    keys.erase(keys.begin() + idx);
    values.erase(values.begin() + idx);
    key_count = static_cast<std::uint16_t>(keys.size());
    return true;
}

bool BLeafPage::set(std::uint64_t key, const Value &value) {
    std::size_t idx = lower_bound_key(key);
    if (idx >= keys.size()) return false;
    if (keys[idx] != key) return false;

    values[idx] = value;
    return true;
}

bool BLeafPage::remove(std::uint64_t key) {
    std::size_t idx = lower_bound_key(key);
    if (idx >= keys.size()) return false;
    if (keys[idx] != key) return false;
    return remove_at(idx);
}

void BLeafPage::flush() {
    // Clear the page first so any stale bytes from older layouts do not survive after flush.
    std::memset(page, 0, 4096);

    // Serialize the fixed-size header first.
    put_u8_be(page, static_cast<std::uint8_t>(page_type));
    put_u16_be(&page[1], static_cast<std::uint16_t>(keys.size()));

    // The leaf header is 7 bytes long. The cell directory starts immediately after it
    // and contributes 2 bytes per key-value cell.
    put_u16_be(&page[3], static_cast<std::uint16_t>(7 + keys.size() * 2));

    std::uint16_t total_cell_bytes = 0;
    for (std::size_t i = 0; i < keys.size(); i++) {
        total_cell_bytes = static_cast<std::uint16_t>(total_cell_bytes + 11 + values[i].size);
    }
    put_u16_be(&page[5], static_cast<std::uint16_t>(4096 - total_cell_bytes - 1));

    // The directory is stored in key order. Each 2-byte entry points at the start of one
    // packed leaf cell body near the end of the page.
    std::uint16_t dir_ptr = 7;
    std::uint16_t cell_ptr = 4096;

    for (std::size_t i = 0; i < keys.size(); i++) {
        std::uint16_t cell_size = 11 + values[i].size;
        cell_ptr = cell_ptr - cell_size;

        // Leaf cell format: 8-byte key, 1-byte ValueType, 2-byte value size, then payload bytes.
        put_u64_be(&page[cell_ptr], keys[i]);
        put_u8_be(&page[cell_ptr + 8], static_cast<std::uint8_t>(values[i].type));
        put_u16_be(&page[cell_ptr + 9], static_cast<std::uint16_t>(values[i].size));
        std::memcpy(&page[cell_ptr + 11], values[i].data.data(), values[i].size);

        // Store the offset to this cell in the directory entry for the same logical key index.
        put_u16_be(&page[dir_ptr], cell_ptr);
        dir_ptr += 2;
    }
}

void BLeafPage::parse_leaf_cell(const char *in, std::uint64_t *key, Value *value) {
    // Leaf cell format: 8-byte key, 1-byte ValueType, 2-byte value size, then payload bytes.
    *key = get_u64_be(in);

    std::uint8_t value_type_int = get_u8_be(&in[8]);
    if (value_type_int > static_cast<std::uint8_t>(ValueType::Char)) std::abort(); // TODO: replace with something appropriate
    value->type = static_cast<ValueType>(value_type_int);

    value->size = get_u16_be(&in[9]);
    value->data.assign(&in[11], &in[11] + value->size);
}

void BLeafPage::fill_initial_layout(char *out) {
    std::memset(out, 0, 4096);
    put_u8_be(out, static_cast<std::uint8_t>(PageType::Leaf));
    put_u16_be(&out[1], 0);
    put_u16_be(&out[3], 7);
    put_u16_be(&out[5], 4095);
}
