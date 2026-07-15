#include <gtest/gtest.h>

#include <BTreePage.h>
#include <Endian.h>
#include <KeyCodec.h>
#include <ValueCodec.h>

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

void write_key(char *page, std::uint16_t cell_offset, const Key &key) {
    put_u8_be(&page[cell_offset], static_cast<std::uint8_t>(key.type));
    put_u16_be(&page[cell_offset + 1], static_cast<std::uint16_t>(key.size));
    std::memcpy(&page[cell_offset + 3], key.data.data(), key.size);
}

void write_leaf_cell(char *page, std::uint16_t cell_offset, const Key &key, ValueType value_type, const char *value_data, std::uint16_t value_size) {
    write_key(page, cell_offset, key);
    put_u8_be(&page[cell_offset + 3 + key.size], static_cast<std::uint8_t>(value_type));
    put_u16_be(&page[cell_offset + 4 + key.size], value_size);
    for (std::uint16_t i = 0; i < value_size; i++) {
        page[cell_offset + 6 + key.size + i] = value_data[i];
    }
}

void build_leaf_page(std::array<char, PAGE_SIZE> &page, std::vector<Key> &expected_keys) {
    page.fill(0);
    Value false_char = make_value(ValueType::Char, "F");
    Value true_bool = make_value(ValueType::Bool, "T");
    Value negative_int = valuecodec::make_varint(-5);
    Value cat_value = make_value(ValueType::Char, "CAT");
    Value bytes_value = make_value(ValueType::Char, "BYTES");

    expected_keys = {
        keycodec::make_bool(false),
        keycodec::make_uint64(10),
        keycodec::make_int64(-5),
        keycodec::make_string("cat"),
        keycodec::make_bytes(std::vector<char>{'z', '1'})
    };

    put_u8_be(page.data(), static_cast<std::uint8_t>(PageType::Leaf));
    put_u16_be(&page[1], 5);
    put_u16_be(&page[3], 17);
    put_u16_be(&page[5], 4030);

    put_u16_be(&page[7], 4088);
    put_u16_be(&page[9], 4073);
    put_u16_be(&page[11], 4056);
    put_u16_be(&page[13], 4044);
    put_u16_be(&page[15], 4031);

    write_leaf_cell(page.data(), 4088, expected_keys[0], false_char.type, false_char.data.data(), false_char.size);
    write_leaf_cell(page.data(), 4073, expected_keys[1], true_bool.type, true_bool.data.data(), true_bool.size);
    write_leaf_cell(page.data(), 4056, expected_keys[2], negative_int.type, negative_int.data.data(), negative_int.size);
    write_leaf_cell(page.data(), 4044, expected_keys[3], cat_value.type, cat_value.data.data(), cat_value.size);
    write_leaf_cell(page.data(), 4031, expected_keys[4], bytes_value.type, bytes_value.data.data(), bytes_value.size);
}

void write_internal_cell(char *page, std::uint16_t cell_offset, const Key &key, std::uint32_t right_child_page_num) {
    write_key(page, cell_offset, key);
    put_u32_be(&page[cell_offset + 3 + key.size], right_child_page_num);
}

void build_internal_page(std::array<char, PAGE_SIZE> &page, std::vector<Key> &expected_keys) {
    page.fill(0);

    expected_keys = {
        keycodec::make_bool(false),
        keycodec::make_uint64(10),
        keycodec::make_string("cat")
    };

    put_u8_be(page.data(), static_cast<std::uint8_t>(PageType::Internal));
    put_u16_be(&page[1], 3);
    put_u16_be(&page[3], 17);
    put_u16_be(&page[5], 4062);
    put_u32_be(&page[7], 11);

    put_u16_be(&page[11], 4088);
    put_u16_be(&page[13], 4073);
    put_u16_be(&page[15], 4063);

    write_internal_cell(page.data(), 4088, expected_keys[0], 22);
    write_internal_cell(page.data(), 4073, expected_keys[1], 33);
    write_internal_cell(page.data(), 4063, expected_keys[2], 44);
}

void expect_key_equals(const Key &actual, const Key &expected) {
    EXPECT_EQ(actual.type, expected.type);
    EXPECT_EQ(actual.size, expected.size);
    EXPECT_EQ(keycodec::equal(actual, expected), true);
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
    std::vector<Key> leaf_keys;
    std::vector<Key> internal_keys;
    build_leaf_page(leaf_page, leaf_keys);
    build_internal_page(internal_page, internal_keys);

    EXPECT_EQ(BTreePage::peek_page_type(leaf_page.data()), PageType::Leaf);
    EXPECT_EQ(BTreePage::peek_page_type(internal_page.data()), PageType::Internal);
}

TEST(BTreePageTest, LeafHelpersExposeDecodedKeysAndValues) {
    std::array<char, PAGE_SIZE> page{};
    std::vector<Key> expected_keys;
    build_leaf_page(page, expected_keys);

    BLeafPage leaf_page(page.data());

    EXPECT_EQ(leaf_page.is_leaf(), true);
    EXPECT_EQ(leaf_page.get_key_count(), 5u);
    ASSERT_TRUE(leaf_page.first_key().has_value());
    expect_key_equals(*leaf_page.first_key(), expected_keys[0]);
    ASSERT_TRUE(leaf_page.key_at(1).has_value());
    expect_key_equals(*leaf_page.key_at(1), expected_keys[1]);
    EXPECT_EQ(leaf_page.key_at(9).has_value(), false);

    std::optional<Value> first_value = leaf_page.get_at(0);
    ASSERT_TRUE(first_value.has_value());
    EXPECT_EQ(first_value->type, ValueType::Char);
    EXPECT_EQ(std::string(first_value->data.begin(), first_value->data.end()), "F");

    std::optional<Value> bytes_value = leaf_page.get(expected_keys[4]);
    ASSERT_TRUE(bytes_value.has_value());
    EXPECT_EQ(bytes_value->type, ValueType::Char);
    EXPECT_EQ(std::string(bytes_value->data.begin(), bytes_value->data.end()), "BYTES");
}

TEST(BTreePageTest, LowerBoundKeyReturnsExpectedInsertionPointsAcrossTypes) {
    std::array<char, PAGE_SIZE> page{};
    std::vector<Key> expected_keys;
    build_leaf_page(page, expected_keys);

    BLeafPage leaf_page(page.data());

    EXPECT_EQ(leaf_page.lower_bound_key(keycodec::make_bool(false)), 0u);
    EXPECT_EQ(leaf_page.lower_bound_key(keycodec::make_bool(true)), 1u);
    EXPECT_EQ(leaf_page.lower_bound_key(keycodec::make_uint64(5)), 1u);
    EXPECT_EQ(leaf_page.lower_bound_key(keycodec::make_uint64(10)), 1u);
    EXPECT_EQ(leaf_page.lower_bound_key(keycodec::make_int64(-6)), 2u);
    EXPECT_EQ(leaf_page.lower_bound_key(keycodec::make_string("dog")), 4u);
    EXPECT_EQ(leaf_page.lower_bound_key(keycodec::make_bytes(std::vector<char>{'a'})), 4u);
    EXPECT_EQ(leaf_page.lower_bound_key(keycodec::make_bytes(std::vector<char>{'z', '9'})), 5u);
}

TEST(BTreePageTest, InternalChildrenAreReturnedRelativeToSeparators) {
    std::array<char, PAGE_SIZE> page{};
    std::vector<Key> expected_keys;
    build_internal_page(page, expected_keys);

    BInternalPage internal_page(page.data());

    EXPECT_EQ(internal_page.is_leaf(), false);
    EXPECT_EQ(internal_page.get_key_count(), 3u);
    ASSERT_TRUE(internal_page.key_at(0).has_value());
    expect_key_equals(*internal_page.key_at(0), expected_keys[0]);
    ASSERT_TRUE(internal_page.key_at(2).has_value());
    expect_key_equals(*internal_page.key_at(2), expected_keys[2]);

    ASSERT_TRUE(internal_page.get_leftmost_child().has_value());
    EXPECT_EQ(internal_page.get_leftmost_child().value(), 11u);
    ASSERT_TRUE(internal_page.get_left_child(0).has_value());
    ASSERT_TRUE(internal_page.get_right_child(0).has_value());
    EXPECT_EQ(internal_page.get_left_child(0).value(), 11u);
    EXPECT_EQ(internal_page.get_right_child(0).value(), 22u);
    EXPECT_EQ(internal_page.get_left_child(3).has_value(), false);
}

TEST(BTreePageTest, LeafInsertSetAndRemoveMutateLogicalState) {
    std::array<char, PAGE_SIZE> page{};
    BLeafPage::fill_initial_layout(page.data());
    BLeafPage leaf_page(page.data());

    Key key_false = keycodec::make_bool(false);
    Key key_uint = keycodec::make_uint64(20);
    Key key_string = keycodec::make_string("cat");

    EXPECT_EQ(leaf_page.insert_at(0, key_string, make_value(ValueType::Char, "C")), true);
    EXPECT_EQ(leaf_page.insert_at(0, key_false, make_value(ValueType::Bool, "F")), true);
    EXPECT_EQ(leaf_page.insert_at(1, key_uint, valuecodec::make_varuint(20)), true);
    EXPECT_EQ(leaf_page.insert_at(9, keycodec::make_bytes(std::vector<char>{'z'}), make_value(ValueType::Char, "Z")), false);

    ASSERT_TRUE(leaf_page.key_at(0).has_value());
    expect_key_equals(*leaf_page.key_at(0), key_false);
    ASSERT_TRUE(leaf_page.key_at(1).has_value());
    expect_key_equals(*leaf_page.key_at(1), key_uint);
    ASSERT_TRUE(leaf_page.key_at(2).has_value());
    expect_key_equals(*leaf_page.key_at(2), key_string);

    EXPECT_EQ(leaf_page.set(key_uint, make_value(ValueType::Bool, "T")), true);
    std::optional<Value> updated_value = leaf_page.get(key_uint);
    ASSERT_TRUE(updated_value.has_value());
    EXPECT_EQ(std::string(updated_value->data.begin(), updated_value->data.end()), "T");
    EXPECT_EQ(leaf_page.set(keycodec::make_uint64(99), make_value(ValueType::Char, "X")), false);

    EXPECT_EQ(leaf_page.remove_at(1), true);
    EXPECT_EQ(leaf_page.remove(key_string), true);
    EXPECT_EQ(leaf_page.remove(key_string), false);
    EXPECT_EQ(leaf_page.remove_at(8), false);

    EXPECT_EQ(leaf_page.get_key_count(), 1u);
    ASSERT_TRUE(leaf_page.first_key().has_value());
    expect_key_equals(*leaf_page.first_key(), key_false);
}

TEST(BTreePageTest, InternalMutatorsUpdateKeysAndChildren) {
    std::array<char, PAGE_SIZE> page{};
    BInternalPage::fill_initial_layout(page.data());
    BInternalPage internal_page(page.data());

    Key key_false = keycodec::make_bool(false);
    Key key_uint = keycodec::make_uint64(20);
    Key key_string = keycodec::make_string("cat");

    EXPECT_EQ(internal_page.set_leftmost_child(11), false);
    EXPECT_EQ(internal_page.replace_leftmost_child(77), true);
    ASSERT_TRUE(internal_page.get_leftmost_child().has_value());
    EXPECT_EQ(internal_page.get_leftmost_child().value(), 77u);

    EXPECT_EQ(internal_page.insert_separator_at(0, key_false, 22), true);
    EXPECT_EQ(internal_page.insert_separator_at(1, key_string, 44), true);
    EXPECT_EQ(internal_page.insert_separator_at(1, key_uint, 33), true);
    EXPECT_EQ(internal_page.insert_separator_at(9, keycodec::make_bytes(std::vector<char>{'z'}), 55), false);

    EXPECT_EQ(internal_page.get_key_count(), 3u);
    ASSERT_TRUE(internal_page.key_at(0).has_value());
    expect_key_equals(*internal_page.key_at(0), key_false);
    ASSERT_TRUE(internal_page.key_at(1).has_value());
    expect_key_equals(*internal_page.key_at(1), key_uint);
    ASSERT_TRUE(internal_page.key_at(2).has_value());
    expect_key_equals(*internal_page.key_at(2), key_string);

    Key replacement_key = keycodec::make_string("cow");
    EXPECT_EQ(internal_page.set_separator_key_at(2, replacement_key), true);
    ASSERT_TRUE(internal_page.key_at(2).has_value());
    expect_key_equals(*internal_page.key_at(2), replacement_key);
    EXPECT_EQ(internal_page.set_separator_key_at(8, replacement_key), false);

    EXPECT_EQ(internal_page.remove_separator_at(1), true);
    EXPECT_EQ(internal_page.remove_separator_at(8), false);
    EXPECT_EQ(internal_page.remove(replacement_key), true);
    EXPECT_EQ(internal_page.remove(replacement_key), false);

    EXPECT_EQ(internal_page.get_key_count(), 1u);
    ASSERT_TRUE(internal_page.key_at(0).has_value());
    expect_key_equals(*internal_page.key_at(0), key_false);
    ASSERT_TRUE(internal_page.get_right_child(0).has_value());
    EXPECT_EQ(internal_page.get_right_child(0).value(), 22u);
}

TEST(BTreePageTest, LeafWriteBackRoundTripsIntoFreshObject) {
    std::array<char, PAGE_SIZE> page{};
    BLeafPage::fill_initial_layout(page.data());

    Key key_false = keycodec::make_bool(false);
    Key key_uint = keycodec::make_uint64(20);
    Key key_string = keycodec::make_string("cat");

    {
        BLeafPage leaf_page(page.data());
        ASSERT_EQ(leaf_page.insert_at(0, key_false, make_value(ValueType::Char, "A")), true);
        ASSERT_EQ(leaf_page.insert_at(1, key_uint, make_value(ValueType::Bool, "B")), true);
        ASSERT_EQ(leaf_page.insert_at(2, key_string, valuecodec::make_varint(123456)), true);
        ASSERT_EQ(leaf_page.set(key_uint, make_value(ValueType::Bool, "T")), true);
        leaf_page.write_back();
    }

    BLeafPage decoded(page.data());
    EXPECT_EQ(decoded.get_key_count(), 3u);
    ASSERT_TRUE(decoded.key_at(0).has_value());
    expect_key_equals(*decoded.key_at(0), key_false);
    ASSERT_TRUE(decoded.key_at(1).has_value());
    expect_key_equals(*decoded.key_at(1), key_uint);
    ASSERT_TRUE(decoded.key_at(2).has_value());
    expect_key_equals(*decoded.key_at(2), key_string);

    std::optional<Value> first_value = decoded.get(key_false);
    ASSERT_TRUE(first_value.has_value());
    EXPECT_EQ(first_value->type, ValueType::Char);
    EXPECT_EQ(std::string(first_value->data.begin(), first_value->data.end()), "A");

    std::optional<Value> middle_value = decoded.get(key_uint);
    ASSERT_TRUE(middle_value.has_value());
    EXPECT_EQ(middle_value->type, ValueType::Bool);
    EXPECT_EQ(std::string(middle_value->data.begin(), middle_value->data.end()), "T");

    std::optional<Value> last_value = decoded.get(key_string);
    ASSERT_TRUE(last_value.has_value());
    EXPECT_EQ(last_value->type, ValueType::VarInt);
    std::int64_t decoded_last_value = 0;
    ASSERT_EQ(valuecodec::decode_varint(*last_value, &decoded_last_value), true);
    EXPECT_EQ(decoded_last_value, 123456);
}

TEST(BTreePageTest, InternalWriteBackRoundTripsIntoFreshObject) {
    std::array<char, PAGE_SIZE> page{};
    BInternalPage::fill_initial_layout(page.data());

    Key key_false = keycodec::make_bool(false);
    Key key_uint = keycodec::make_uint64(20);
    Key key_string = keycodec::make_string("cat");

    {
        BInternalPage internal_page(page.data());
        ASSERT_EQ(internal_page.replace_leftmost_child(11), true);
        ASSERT_EQ(internal_page.insert_separator_at(0, key_false, 22), true);
        ASSERT_EQ(internal_page.insert_separator_at(1, key_uint, 33), true);
        ASSERT_EQ(internal_page.insert_separator_at(2, key_string, 44), true);
        ASSERT_EQ(internal_page.set_separator_key_at(1, keycodec::make_uint64(25)), true);
        internal_page.write_back();
    }

    BInternalPage decoded(page.data());
    EXPECT_EQ(decoded.get_key_count(), 3u);
    ASSERT_TRUE(decoded.get_leftmost_child().has_value());
    EXPECT_EQ(decoded.get_leftmost_child().value(), 11u);
    ASSERT_TRUE(decoded.key_at(0).has_value());
    expect_key_equals(*decoded.key_at(0), key_false);
    ASSERT_TRUE(decoded.key_at(1).has_value());
    expect_key_equals(*decoded.key_at(1), keycodec::make_uint64(25));
    ASSERT_TRUE(decoded.key_at(2).has_value());
    expect_key_equals(*decoded.key_at(2), key_string);
    ASSERT_TRUE(decoded.get_right_child(0).has_value());
    EXPECT_EQ(decoded.get_right_child(0).value(), 22u);
    ASSERT_TRUE(decoded.get_right_child(1).has_value());
    EXPECT_EQ(decoded.get_right_child(1).value(), 33u);
    ASSERT_TRUE(decoded.get_right_child(2).has_value());
    EXPECT_EQ(decoded.get_right_child(2).value(), 44u);
}

} // namespace
