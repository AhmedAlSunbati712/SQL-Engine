#include <gtest/gtest.h>

#include <KeyCodec.h>
#include <NetCodec.h>
#include <ValueCodec.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

TEST(NetCodecTest, SerializeKeyUsesTypeSizeAndRawBytes) {
    const Key key = KeyCodec::make_uint64(0x0102030405060708ULL);
    const std::vector<std::uint8_t> encoded = NetCodec::serialize_key(key);

    const std::vector<std::uint8_t> expected = {
        static_cast<std::uint8_t>(KeyType::UInt64),
        0, 0, 0, 8,
        1, 2, 3, 4, 5, 6, 7, 8
    };
    EXPECT_EQ(encoded, expected);
}

TEST(NetCodecTest, KeyTypesRoundTrip) {
    const std::vector<Key> keys = {
        KeyCodec::make_bool(true),
        KeyCodec::make_uint64(42),
        KeyCodec::make_int64(-42),
        KeyCodec::make_string(std::string{"key\0text", 8}),
        KeyCodec::make_bytes(std::vector<char>{'b', '\0', 'y'})
    };

    for (const Key& key : keys) {
        const Key decoded = NetCodec::deserialize_key(NetCodec::serialize_key(key));
        EXPECT_TRUE(KeyCodec::equal(decoded, key));
    }
}

TEST(NetCodecTest, SerializeKeyRejectsInvalidInput) {
    Key invalid = KeyCodec::make_string("key");
    invalid.size = 4;

    EXPECT_THROW(NetCodec::serialize_key(invalid), std::invalid_argument);
}

TEST(NetCodecTest, DeserializeKeyRejectsMalformedBuffers) {
    EXPECT_THROW(NetCodec::deserialize_key(std::vector<std::uint8_t>(4, 0)), std::runtime_error);

    const std::vector<std::uint8_t> unknown_type = {99, 0, 0, 0, 0};
    EXPECT_THROW(NetCodec::deserialize_key(unknown_type), std::runtime_error);

    const std::vector<std::uint8_t> wrong_size = {
        static_cast<std::uint8_t>(KeyType::String), 0, 0, 0, 2, 'x'
    };
    EXPECT_THROW(NetCodec::deserialize_key(wrong_size), std::runtime_error);

    const std::vector<std::uint8_t> invalid_bool = {
        static_cast<std::uint8_t>(KeyType::Bool), 0, 0, 0, 1, 'x'
    };
    EXPECT_THROW(NetCodec::deserialize_key(invalid_bool), std::runtime_error);
}

TEST(NetCodecTest, SerializeValueUsesTypeSizeAndRawBytes) {
    const Value value = ValueCodec::make_char("value");
    const std::vector<std::uint8_t> encoded = NetCodec::serialize_value(value);

    const std::vector<std::uint8_t> expected = {
        static_cast<std::uint8_t>(ValueType::Char),
        0, 0, 0, 5,
        'v', 'a', 'l', 'u', 'e'
    };
    EXPECT_EQ(encoded, expected);
}

TEST(NetCodecTest, ValueTypesRoundTrip) {
    const std::vector<Value> values = {
        ValueCodec::make_varuint(300),
        ValueCodec::make_varint(-300),
        ValueCodec::make_bool(true),
        ValueCodec::make_char(std::string{"value\0text", 10})
    };

    for (const Value& value : values) {
        const Value decoded = NetCodec::deserialize_value(NetCodec::serialize_value(value));
        EXPECT_TRUE(ValueCodec::equal(decoded, value));
    }
}

TEST(NetCodecTest, SerializeValueRejectsInvalidInput) {
    Value invalid = ValueCodec::make_char("value");
    invalid.size = 6;

    EXPECT_THROW(NetCodec::serialize_value(invalid), std::invalid_argument);
}

TEST(NetCodecTest, DeserializeValueRejectsMalformedBuffers) {
    EXPECT_THROW(NetCodec::deserialize_value(std::vector<std::uint8_t>(4, 0)), std::runtime_error);

    const std::vector<std::uint8_t> unknown_type = {99, 0, 0, 0, 0};
    EXPECT_THROW(NetCodec::deserialize_value(unknown_type), std::runtime_error);

    const std::vector<std::uint8_t> wrong_size = {
        static_cast<std::uint8_t>(ValueType::Char), 0, 0, 0, 2, 'x'
    };
    EXPECT_THROW(NetCodec::deserialize_value(wrong_size), std::runtime_error);

    const std::vector<std::uint8_t> invalid_bool = {
        static_cast<std::uint8_t>(ValueType::Bool), 0, 0, 0, 1, 'x'
    };
    EXPECT_THROW(NetCodec::deserialize_value(invalid_bool), std::runtime_error);

    const std::vector<std::uint8_t> unterminated_varuint = {
        static_cast<std::uint8_t>(ValueType::VarUInt), 0, 0, 0, 1, 0x80
    };
    EXPECT_THROW(NetCodec::deserialize_value(unterminated_varuint), std::runtime_error);
}

} // namespace
