#include <gtest/gtest.h>

#include <KeyCodec.h>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace {

TEST(KeyCodecTest, TypePrecedenceDefinesCrossTypeOrder) {
    Key bool_key = keycodec::make_bool(false);
    Key uint_key = keycodec::make_uint64(0);
    Key int_key = keycodec::make_int64(0);
    Key string_key = keycodec::make_string("a");
    Key bytes_key = keycodec::make_bytes(std::vector<char>{'a'});

    EXPECT_LT(keycodec::compare(bool_key, uint_key), 0);
    EXPECT_LT(keycodec::compare(uint_key, int_key), 0);
    EXPECT_LT(keycodec::compare(int_key, string_key), 0);
    EXPECT_LT(keycodec::compare(string_key, bytes_key), 0);
}

TEST(KeyCodecTest, UInt64KeysCompareByNumericOrder) {
    Key small_key = keycodec::make_uint64(10);
    Key large_key = keycodec::make_uint64(100);

    EXPECT_LT(keycodec::compare(small_key, large_key), 0);
    EXPECT_GT(keycodec::compare(large_key, small_key), 0);
}

TEST(KeyCodecTest, Int64KeysCompareByNumericOrder) {
    Key negative_key = keycodec::make_int64(-5);
    Key zero_key = keycodec::make_int64(0);
    Key positive_key = keycodec::make_int64(9);

    EXPECT_LT(keycodec::compare(negative_key, zero_key), 0);
    EXPECT_LT(keycodec::compare(zero_key, positive_key), 0);
}

TEST(KeyCodecTest, StringsAndBytesCompareLexicographically) {
    Key cat_key = keycodec::make_string("cat");
    Key dog_key = keycodec::make_string("dog");
    Key bytes_a = keycodec::make_bytes(std::vector<char>{'a'});
    Key bytes_b = keycodec::make_bytes(std::vector<char>{'b'});

    EXPECT_LT(keycodec::compare(cat_key, dog_key), 0);
    EXPECT_LT(keycodec::compare(bytes_a, bytes_b), 0);
}

TEST(KeyCodecTest, EqualRequiresSameTypeAndPayload) {
    Key uint_five = keycodec::make_uint64(5);
    Key int_five = keycodec::make_int64(5);
    Key uint_five_copy = keycodec::make_uint64(5);

    EXPECT_EQ(keycodec::equal(uint_five, uint_five_copy), true);
    EXPECT_EQ(keycodec::equal(uint_five, int_five), false);
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

    EXPECT_EQ(keycodec::validate_key(bad_bool), false);
    EXPECT_EQ(keycodec::validate_key(bad_uint), false);
    EXPECT_EQ(keycodec::validate_key(bad_int), false);
}

TEST(KeyCodecTest, ValidateKeyAcceptsFactoryOutput) {
    EXPECT_EQ(keycodec::validate_key(keycodec::make_bool(true)), true);
    EXPECT_EQ(keycodec::validate_key(keycodec::make_uint64(123)), true);
    EXPECT_EQ(keycodec::validate_key(keycodec::make_int64(-123)), true);
    EXPECT_EQ(keycodec::validate_key(keycodec::make_string("hello")), true);
    EXPECT_EQ(keycodec::validate_key(keycodec::make_bytes(std::vector<char>{'x', 'y'})), true);
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
        std::optional<Key> encoded = keycodec::encode(input);
        ASSERT_TRUE(encoded.has_value());
        EXPECT_TRUE(keycodec::validate_key(*encoded));

        std::optional<KeyInput> decoded = keycodec::decode(*encoded);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, input);
    }
}

TEST(KeyCodecTest, EncodeRejectsPayloadThatLeafCellsCannotRepresent) {
    const std::string oversized(keycodec::MAX_PAYLOAD_SIZE + 1, 'x');
    EXPECT_FALSE(keycodec::encode(KeyInput{oversized}).has_value());

    Key raw = keycodec::make_string(oversized);
    EXPECT_FALSE(keycodec::validate_key(raw));
    EXPECT_FALSE(keycodec::decode(raw).has_value());
}

} // namespace
