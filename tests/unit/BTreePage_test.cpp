#include <gtest/gtest.h>

#include <BTreePage.h>
#include <Endian.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr std::size_t PAGE_SIZE = 4096;

Value make_value(ValueType type, const std::string &payload) {
    Value value{};
    value.type = type;
    value.size = static_cast<std::uint32_t>(payload.size());
    value.data.assign(payload.begin(), payload.end());
    return value;
}

void write_leaf_cell(char *page, std::uint16_t cell_offset, std::uint64_t key, ValueType value_type, const char *value_data, std::uint16_t value_size) {
    put_u64_be(&page[cell_offset], key);
    put_u8_be(&page[cell_offset + 8], static_cast<std::uint8_t>(value_type));
    put_u16_be(&page[cell_offset + 9], value_size);
    for (std::uint16_t i = 0; i < value_size; i++) {
        page[cell_offset + 11 + i] = value_data[i];
    }
}

void build_leaf_page(std::array<char, PAGE_SIZE> &page) {
    page.fill(0);

    put_u8_be(page.data(), static_cast<std::uint8_t>(PageType::Leaf));
    put_u16_be(&page[1], 3);
    put_u16_be(&page[3], 13);
    put_u16_be(&page[5], 4052);

    put_u16_be(&page[7], 4082);
    put_u16_be(&page[9], 4069);
    put_u16_be(&page[11], 4052);

    write_leaf_cell(page.data(), 4082, 10, ValueType::Char, "A", 1);
    write_leaf_cell(page.data(), 4069, 20, ValueType::Bool, "B", 1);
    write_leaf_cell(page.data(), 4052, 40, ValueType::VarInt, "CDEFG", 5);
}

void write_internal_cell(char *page, std::uint16_t cell_offset, std::uint64_t key, std::uint32_t right_child_page_num) {
    put_u64_be(&page[cell_offset], key);
    put_u32_be(&page[cell_offset + 8], right_child_page_num);
}

void build_internal_page(std::array<char, PAGE_SIZE> &page) {
    page.fill(0);

    put_u8_be(page.data(), static_cast<std::uint8_t>(PageType::Internal));
    put_u16_be(&page[1], 3);
    put_u16_be(&page[3], 17);
    put_u16_be(&page[5], 4059);
    put_u32_be(&page[7], 11);

    put_u16_be(&page[11], 4084);
    put_u16_be(&page[13], 4072);
    put_u16_be(&page[15], 4060);

    write_internal_cell(page.data(), 4084, 10, 22);
    write_internal_cell(page.data(), 4072, 20, 33);
    write_internal_cell(page.data(), 4060, 40, 44);
}

TEST(BTreePageTest, FillInitialLayoutCreatesEmptyLeafPage) {
    std::array<char, PAGE_SIZE> page{};

    BLeafPage::fill_initial_layout(page.data());
    BLeafPage leaf_page(page.data());

    EXPECT_EQ(BTreePage::peek_page_type(page.data()), PageType::Leaf);
    EXPECT_EQ(leaf_page.is_leaf(), true);
    EXPECT_EQ(leaf_page.get_key_count(), 0u);
    EXPECT_EQ(leaf_page.first_key().has_value(), false);
}

TEST(BTreePageTest, FillInitialLayoutCreatesEmptyInternalPage) {
    std::array<char, PAGE_SIZE> page{};

    BInternalPage::fill_initial_layout(page.data());
    BInternalPage internal_page(page.data());

    EXPECT_EQ(BTreePage::peek_page_type(page.data()), PageType::Internal);
    EXPECT_EQ(internal_page.is_leaf(), false);
    EXPECT_EQ(internal_page.get_key_count(), 0u);
    EXPECT_EQ(internal_page.first_key().has_value(), false);
    ASSERT_TRUE(internal_page.get_leftmost_child().has_value());
    EXPECT_EQ(internal_page.get_leftmost_child().value(), 0u);
}

TEST(BTreePageTest, PeekPageTypeReadsLeafAndInternalPages) {
    std::array<char, PAGE_SIZE> leaf_page{};
    std::array<char, PAGE_SIZE> internal_page{};
    build_leaf_page(leaf_page);
    build_internal_page(internal_page);

    EXPECT_EQ(BTreePage::peek_page_type(leaf_page.data()), PageType::Leaf);
    EXPECT_EQ(BTreePage::peek_page_type(internal_page.data()), PageType::Internal);
}

TEST(BTreePageTest, LeafHelpersExposeDecodedKeysAndValues) {
    std::array<char, PAGE_SIZE> page{};
    build_leaf_page(page);

    BLeafPage leaf_page(page.data());

    EXPECT_EQ(leaf_page.is_leaf(), true);
    EXPECT_EQ(leaf_page.get_key_count(), 3u);
    ASSERT_TRUE(leaf_page.first_key().has_value());
    EXPECT_EQ(leaf_page.first_key().value(), 10u);
    ASSERT_TRUE(leaf_page.key_at(1).has_value());
    EXPECT_EQ(leaf_page.key_at(1).value(), 20u);
    EXPECT_EQ(leaf_page.key_at(3).has_value(), false);

    std::optional<Value> first_value = leaf_page.get_at(0);
    ASSERT_TRUE(first_value.has_value());
    EXPECT_EQ(first_value->type, ValueType::Char);
    EXPECT_EQ(std::string(first_value->data.begin(), first_value->data.end()), "A");

    std::optional<Value> last_value = leaf_page.get(40);
    ASSERT_TRUE(last_value.has_value());
    EXPECT_EQ(last_value->type, ValueType::VarInt);
    EXPECT_EQ(std::string(last_value->data.begin(), last_value->data.end()), "CDEFG");
    EXPECT_EQ(leaf_page.get(99).has_value(), false);
    EXPECT_EQ(leaf_page.get_at(9).has_value(), false);
}

TEST(BTreePageTest, LowerBoundKeyReturnsExpectedInsertionPoints) {
    std::array<char, PAGE_SIZE> page{};
    build_leaf_page(page);

    BLeafPage leaf_page(page.data());

    EXPECT_EQ(leaf_page.lower_bound_key(5), 0u);
    EXPECT_EQ(leaf_page.lower_bound_key(10), 0u);
    EXPECT_EQ(leaf_page.lower_bound_key(19), 1u);
    EXPECT_EQ(leaf_page.lower_bound_key(20), 1u);
    EXPECT_EQ(leaf_page.lower_bound_key(21), 2u);
    EXPECT_EQ(leaf_page.lower_bound_key(99), 3u);
}

TEST(BTreePageTest, InternalChildrenAreReturnedRelativeToSeparators) {
    std::array<char, PAGE_SIZE> page{};
    build_internal_page(page);

    BInternalPage internal_page(page.data());

    EXPECT_EQ(internal_page.is_leaf(), false);
    EXPECT_EQ(internal_page.get_key_count(), 3u);
    ASSERT_TRUE(internal_page.get_leftmost_child().has_value());
    EXPECT_EQ(internal_page.get_leftmost_child().value(), 11u);
    ASSERT_TRUE(internal_page.get_left_child(0).has_value());
    ASSERT_TRUE(internal_page.get_right_child(0).has_value());
    ASSERT_TRUE(internal_page.get_left_child(1).has_value());
    ASSERT_TRUE(internal_page.get_right_child(1).has_value());
    EXPECT_EQ(internal_page.get_left_child(0).value(), 11u);
    EXPECT_EQ(internal_page.get_right_child(0).value(), 22u);
    EXPECT_EQ(internal_page.get_left_child(1).value(), 22u);
    EXPECT_EQ(internal_page.get_right_child(1).value(), 33u);
    EXPECT_EQ(internal_page.get_left_child(3).has_value(), false);
    EXPECT_EQ(internal_page.get_right_child(3).has_value(), false);
}

TEST(BTreePageTest, LeafInsertSetAndRemoveMutateLogicalState) {
    std::array<char, PAGE_SIZE> page{};
    BLeafPage::fill_initial_layout(page.data());
    BLeafPage leaf_page(page.data());

    EXPECT_EQ(leaf_page.insert_at(0, 20, make_value(ValueType::Bool, "B")), true);
    EXPECT_EQ(leaf_page.insert_at(0, 10, make_value(ValueType::Char, "A")), true);
    EXPECT_EQ(leaf_page.insert_at(2, 40, make_value(ValueType::VarInt, "CDEFG")), true);
    EXPECT_EQ(leaf_page.insert_at(9, 50, make_value(ValueType::Char, "Z")), false);

    ASSERT_TRUE(leaf_page.key_at(0).has_value());
    EXPECT_EQ(leaf_page.key_at(0).value(), 10u);
    ASSERT_TRUE(leaf_page.key_at(1).has_value());
    EXPECT_EQ(leaf_page.key_at(1).value(), 20u);
    ASSERT_TRUE(leaf_page.key_at(2).has_value());
    EXPECT_EQ(leaf_page.key_at(2).value(), 40u);

    EXPECT_EQ(leaf_page.set(20, make_value(ValueType::Bool, "T")), true);
    std::optional<Value> updated_value = leaf_page.get(20);
    ASSERT_TRUE(updated_value.has_value());
    EXPECT_EQ(std::string(updated_value->data.begin(), updated_value->data.end()), "T");
    EXPECT_EQ(leaf_page.set(99, make_value(ValueType::Char, "X")), false);

    EXPECT_EQ(leaf_page.remove_at(1), true);
    EXPECT_EQ(leaf_page.remove(40), true);
    EXPECT_EQ(leaf_page.remove(40), false);
    EXPECT_EQ(leaf_page.remove_at(8), false);

    EXPECT_EQ(leaf_page.get_key_count(), 1u);
    ASSERT_TRUE(leaf_page.first_key().has_value());
    EXPECT_EQ(leaf_page.first_key().value(), 10u);
}

TEST(BTreePageTest, InternalMutatorsUpdateKeysAndChildren) {
    std::array<char, PAGE_SIZE> page{};
    BInternalPage::fill_initial_layout(page.data());
    BInternalPage internal_page(page.data());

    EXPECT_EQ(internal_page.set_leftmost_child(11), false);
    EXPECT_EQ(internal_page.replace_leftmost_child(77), true);
    ASSERT_TRUE(internal_page.get_leftmost_child().has_value());
    EXPECT_EQ(internal_page.get_leftmost_child().value(), 77u);

    EXPECT_EQ(internal_page.insert_separator_at(0, 10, 22), true);
    EXPECT_EQ(internal_page.insert_separator_at(1, 40, 44), true);
    EXPECT_EQ(internal_page.insert_separator_at(1, 20, 33), true);
    EXPECT_EQ(internal_page.insert_separator_at(9, 50, 55), false);

    EXPECT_EQ(internal_page.get_key_count(), 3u);
    ASSERT_TRUE(internal_page.key_at(0).has_value());
    EXPECT_EQ(internal_page.key_at(0).value(), 10u);
    ASSERT_TRUE(internal_page.key_at(1).has_value());
    EXPECT_EQ(internal_page.key_at(1).value(), 20u);
    ASSERT_TRUE(internal_page.key_at(2).has_value());
    EXPECT_EQ(internal_page.key_at(2).value(), 40u);

    EXPECT_EQ(internal_page.set_separator_key_at(1, 25), true);
    ASSERT_TRUE(internal_page.key_at(1).has_value());
    EXPECT_EQ(internal_page.key_at(1).value(), 25u);
    EXPECT_EQ(internal_page.set_separator_key_at(8, 60), false);

    EXPECT_EQ(internal_page.remove_separator_at(1), true);
    EXPECT_EQ(internal_page.remove_separator_at(8), false);
    EXPECT_EQ(internal_page.remove(40), true);
    EXPECT_EQ(internal_page.remove(40), false);

    EXPECT_EQ(internal_page.get_key_count(), 1u);
    ASSERT_TRUE(internal_page.key_at(0).has_value());
    EXPECT_EQ(internal_page.key_at(0).value(), 10u);
    ASSERT_TRUE(internal_page.get_right_child(0).has_value());
    EXPECT_EQ(internal_page.get_right_child(0).value(), 22u);
}

TEST(BTreePageTest, LeafWriteBackRoundTripsIntoFreshObject) {
    std::array<char, PAGE_SIZE> page{};
    BLeafPage::fill_initial_layout(page.data());

    {
        BLeafPage leaf_page(page.data());
        ASSERT_EQ(leaf_page.insert_at(0, 10, make_value(ValueType::Char, "A")), true);
        ASSERT_EQ(leaf_page.insert_at(1, 20, make_value(ValueType::Bool, "B")), true);
        ASSERT_EQ(leaf_page.insert_at(2, 40, make_value(ValueType::VarInt, "123456")), true);
        ASSERT_EQ(leaf_page.set(20, make_value(ValueType::Bool, "T")), true);
        leaf_page.write_back();
    }

    BLeafPage decoded(page.data());
    EXPECT_EQ(decoded.get_key_count(), 3u);
    ASSERT_TRUE(decoded.key_at(0).has_value());
    EXPECT_EQ(decoded.key_at(0).value(), 10u);
    ASSERT_TRUE(decoded.key_at(1).has_value());
    EXPECT_EQ(decoded.key_at(1).value(), 20u);
    ASSERT_TRUE(decoded.key_at(2).has_value());
    EXPECT_EQ(decoded.key_at(2).value(), 40u);

    std::optional<Value> first_value = decoded.get(10);
    ASSERT_TRUE(first_value.has_value());
    EXPECT_EQ(first_value->type, ValueType::Char);
    EXPECT_EQ(std::string(first_value->data.begin(), first_value->data.end()), "A");

    std::optional<Value> middle_value = decoded.get(20);
    ASSERT_TRUE(middle_value.has_value());
    EXPECT_EQ(middle_value->type, ValueType::Bool);
    EXPECT_EQ(std::string(middle_value->data.begin(), middle_value->data.end()), "T");

    std::optional<Value> last_value = decoded.get(40);
    ASSERT_TRUE(last_value.has_value());
    EXPECT_EQ(last_value->type, ValueType::VarInt);
    EXPECT_EQ(std::string(last_value->data.begin(), last_value->data.end()), "123456");
}

TEST(BTreePageTest, InternalWriteBackRoundTripsIntoFreshObject) {
    std::array<char, PAGE_SIZE> page{};
    BInternalPage::fill_initial_layout(page.data());

    {
        BInternalPage internal_page(page.data());
        ASSERT_EQ(internal_page.replace_leftmost_child(11), true);
        ASSERT_EQ(internal_page.insert_separator_at(0, 10, 22), true);
        ASSERT_EQ(internal_page.insert_separator_at(1, 20, 33), true);
        ASSERT_EQ(internal_page.insert_separator_at(2, 40, 44), true);
        ASSERT_EQ(internal_page.set_separator_key_at(1, 25), true);
        internal_page.write_back();
    }

    BInternalPage decoded(page.data());
    EXPECT_EQ(decoded.get_key_count(), 3u);
    ASSERT_TRUE(decoded.get_leftmost_child().has_value());
    EXPECT_EQ(decoded.get_leftmost_child().value(), 11u);
    ASSERT_TRUE(decoded.key_at(0).has_value());
    EXPECT_EQ(decoded.key_at(0).value(), 10u);
    ASSERT_TRUE(decoded.key_at(1).has_value());
    EXPECT_EQ(decoded.key_at(1).value(), 25u);
    ASSERT_TRUE(decoded.key_at(2).has_value());
    EXPECT_EQ(decoded.key_at(2).value(), 40u);
    ASSERT_TRUE(decoded.get_right_child(0).has_value());
    EXPECT_EQ(decoded.get_right_child(0).value(), 22u);
    ASSERT_TRUE(decoded.get_right_child(1).has_value());
    EXPECT_EQ(decoded.get_right_child(1).value(), 33u);
    ASSERT_TRUE(decoded.get_right_child(2).has_value());
    EXPECT_EQ(decoded.get_right_child(2).value(), 44u);
}

} // namespace
