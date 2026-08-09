#include <gtest/gtest.h>

#include <KeyCodec.h>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace {

TEST(KeyCodecTest, TypePrecedenceDefinesCrossTypeOrder) {
    Key bool_key = KeyCodec::make_bool(false);
    Key uint_key = KeyCodec::make_uint64(0);
    Key int_key = KeyCodec::make_int64(0);
    Key string_key = KeyCodec::make_string("a");
    Key bytes_key = KeyCodec::make_bytes(std::vector<char>{'a'});

    EXPECT_LT(KeyCodec::compare(bool_key, uint_key), 0);
    EXPECT_LT(KeyCodec::compare(uint_key, int_key), 0);
    EXPECT_LT(KeyCodec::compare(int_key, string_key), 0);
    EXPECT_LT(KeyCodec::compare(string_key, bytes_key), 0);
}

TEST(KeyCodecTest, UInt64KeysCompareByNumericOrder) {
    Key small_key = KeyCodec::make_uint64(10);
    Key large_key = KeyCodec::make_uint64(100);

    EXPECT_LT(KeyCodec::compare(small_key, large_key), 0);
    EXPECT_GT(KeyCodec::compare(large_key, small_key), 0);
}

TEST(KeyCodecTest, Int64KeysCompareByNumericOrder) {
    Key negative_key = KeyCodec::make_int64(-5);
    Key zero_key = KeyCodec::make_int64(0);
    Key positive_key = KeyCodec::make_int64(9);

    EXPECT_LT(KeyCodec::compare(negative_key, zero_key), 0);
    EXPECT_LT(KeyCodec::compare(zero_key, positive_key), 0);
}

TEST(KeyCodecTest, StringsAndBytesCompareLexicographically) {
    Key cat_key = KeyCodec::make_string("cat");
    Key dog_key = KeyCodec::make_string("dog");
    Key bytes_a = KeyCodec::make_bytes(std::vector<char>{'a'});
    Key bytes_b = KeyCodec::make_bytes(std::vector<char>{'b'});

    EXPECT_LT(KeyCodec::compare(cat_key, dog_key), 0);
    EXPECT_LT(KeyCodec::compare(bytes_a, bytes_b), 0);
}

TEST(KeyCodecTest, EqualRequiresSameTypeAndPayload) {
    Key uint_five = KeyCodec::make_uint64(5);
    Key int_five = KeyCodec::make_int64(5);
    Key uint_five_copy = KeyCodec::make_uint64(5);

    EXPECT_EQ(KeyCodec::equal(uint_five, uint_five_copy), true);
    EXPECT_EQ(KeyCodec::equal(uint_five, int_five), false);
}

TEST(KeyCodecTest, ValidateKeyRejectsMalformedFixedWidthPayloads) {
    Key bad_bool{};
    bad_bool.type = KeyType::Bool;
    bad_bool.size = 2;
    bad_bool.data = {'\0', '\1'};

    Key bad_uint{};
    bad_uint.type = KeyType::UInt64;
    bad_uint.size = 7;
    bad_uint.data = std::vector<char>(7, '\0');

    Key bad_int{};
    bad_int.type = KeyType::Int64;
    bad_int.size = 9;
    bad_int.data = std::vector<char>(9, '\0');

    EXPECT_EQ(KeyCodec::validate_key(bad_bool), false);
    EXPECT_EQ(KeyCodec::validate_key(bad_uint), false);
    EXPECT_EQ(KeyCodec::validate_key(bad_int), false);
}

TEST(KeyCodecTest, ValidateKeyAcceptsFactoryOutput) {
    EXPECT_EQ(KeyCodec::validate_key(KeyCodec::make_bool(true)), true);
    EXPECT_EQ(KeyCodec::validate_key(KeyCodec::make_uint64(123)), true);
    EXPECT_EQ(KeyCodec::validate_key(KeyCodec::make_int64(-123)), true);
    EXPECT_EQ(KeyCodec::validate_key(KeyCodec::make_string("hello")), true);
    EXPECT_EQ(KeyCodec::validate_key(KeyCodec::make_bytes(std::vector<char>{'x', 'y'})), true);
}

TEST(KeyCodecTest, PrimitiveInputsRoundTripThroughCanonicalEncoding) {
    const std::vector<KeyInput> inputs = {
        KeyInput{true},
        KeyInput{std::numeric_limits<std::uint64_t>::max()},
        KeyInput{std::numeric_limits<std::int64_t>::min()},
        KeyInput{std::string{"key\0text", 8}},
        KeyInput{std::vector<char>{'b', '\0', 'y'}}
    };

    for (const KeyInput &input : inputs) {
        std::optional<Key> encoded = KeyCodec::encode(input);
        ASSERT_TRUE(encoded.has_value());
        EXPECT_TRUE(KeyCodec::validate_key(*encoded));

        std::optional<KeyInput> decoded = KeyCodec::decode(*encoded);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, input);
    }
}

TEST(KeyCodecTest, EncodeRejectsPayloadThatLeafCellsCannotRepresent) {
    const std::string oversized(KeyCodec::MAX_PAYLOAD_SIZE + 1, 'x');
    EXPECT_FALSE(KeyCodec::encode(KeyInput{oversized}).has_value());

    Key raw = KeyCodec::make_string(oversized);
    EXPECT_FALSE(KeyCodec::validate_key(raw));
    EXPECT_FALSE(KeyCodec::decode(raw).has_value());
}

} // namespace
