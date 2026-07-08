#include <gtest/gtest.h>

#include <BTreePage.h>
#include <Endian.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

constexpr std::size_t PAGE_SIZE = 4096;

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

TEST(BTreePageTest, PeekPageTypeReadsLeafAndInternalPages) {
    std::array<char, PAGE_SIZE> leaf_page{};
    std::array<char, PAGE_SIZE> internal_page{};
    build_leaf_page(leaf_page);
    build_internal_page(internal_page);

    EXPECT_EQ(BTreePage::peek_page_type(leaf_page.data()), PageType::Leaf);
    EXPECT_EQ(BTreePage::peek_page_type(internal_page.data()), PageType::Internal);
}

TEST(BTreePageTest, LeafHelpersExposeDecodedKeys) {
    std::array<char, PAGE_SIZE> page{};
    build_leaf_page(page);

    BLeafPage leaf_page(page.data());

    EXPECT_EQ(leaf_page.is_leaf(), true);
    ASSERT_TRUE(leaf_page.first_key().has_value());
    EXPECT_EQ(leaf_page.first_key().value(), 10u);
    ASSERT_TRUE(leaf_page.key_at(1).has_value());
    EXPECT_EQ(leaf_page.key_at(1).value(), 20u);
    EXPECT_EQ(leaf_page.key_at(3).has_value(), false);
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

TEST(BTreePageTest, RemoveDeletesMatchingInternalSeparator) {
    std::array<char, PAGE_SIZE> page{};
    build_internal_page(page);

    BInternalPage internal_page(page.data());

    EXPECT_EQ(internal_page.remove(20), true);
    EXPECT_EQ(internal_page.remove(20), false);

    ASSERT_TRUE(internal_page.first_key().has_value());
    EXPECT_EQ(internal_page.first_key().value(), 10u);
    ASSERT_TRUE(internal_page.key_at(1).has_value());
    EXPECT_EQ(internal_page.key_at(1).value(), 40u);
    EXPECT_EQ(internal_page.key_at(2).has_value(), false);
    ASSERT_TRUE(internal_page.get_right_child(0).has_value());
    EXPECT_EQ(internal_page.get_right_child(0).value(), 22u);
    ASSERT_TRUE(internal_page.get_right_child(1).has_value());
    EXPECT_EQ(internal_page.get_right_child(1).value(), 44u);
}

} // namespace
