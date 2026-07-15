#include <gtest/gtest.h>

#include <ValueCodec.h>

#include <vector>

namespace {

TEST(ValueCodecTest, EqualRequiresSameTypeSizeAndPayload) {
    Value lhs = valuecodec::make_char("cat");
    Value rhs = valuecodec::make_char("cat");
    Value different_type = valuecodec::make_varuint(7);
    Value different_payload = valuecodec::make_char("dog");

    EXPECT_EQ(valuecodec::equal(lhs, rhs), true);
    EXPECT_EQ(valuecodec::equal(lhs, different_type), false);
    EXPECT_EQ(valuecodec::equal(lhs, different_payload), false);
}

TEST(ValueCodecTest, ValidateValueRejectsMalformedShapes) {
    Value bad_bool{};
    bad_bool.type = ValueType::Bool;
    bad_bool.size = 2;
    bad_bool.data = {'T', 'F'};

    Value bad_varuint{};
    bad_varuint.type = ValueType::VarUInt;
    bad_varuint.size = 2;
    bad_varuint.data = {static_cast<char>(0x80), static_cast<char>(0x80)};

    Value bad_varint{};
    bad_varint.type = ValueType::VarInt;
    bad_varint.size = 1;
    bad_varint.data = {static_cast<char>(0x80)};

    Value bad_size{};
    bad_size.type = ValueType::Char;
    bad_size.size = 4;
    bad_size.data = {'c', 'a', 't'};

    EXPECT_EQ(valuecodec::validate_value(bad_varuint), false);
    EXPECT_EQ(valuecodec::validate_value(bad_bool), false);
    EXPECT_EQ(valuecodec::validate_value(bad_varint), false);
    EXPECT_EQ(valuecodec::validate_value(bad_size), false);
}

TEST(ValueCodecTest, ValidateValueAcceptsFactoryOutput) {
    EXPECT_EQ(valuecodec::validate_value(valuecodec::make_varuint(123)), true);
    EXPECT_EQ(valuecodec::validate_value(valuecodec::make_varint(-123)), true);
    EXPECT_EQ(valuecodec::validate_value(valuecodec::make_bool(true)), true);
    EXPECT_EQ(valuecodec::validate_value(valuecodec::make_char("hello")), true);
}

TEST(ValueCodecTest, BoolFactoryEncodesSingleBytePayload) {
    Value false_value = valuecodec::make_bool(false);
    Value true_value = valuecodec::make_bool(true);

    ASSERT_EQ(false_value.type, ValueType::Bool);
    ASSERT_EQ(false_value.size, 1u);
    ASSERT_EQ(false_value.data.size(), 1u);
    EXPECT_EQ(false_value.data[0], '\0');

    ASSERT_EQ(true_value.type, ValueType::Bool);
    ASSERT_EQ(true_value.size, 1u);
    ASSERT_EQ(true_value.data.size(), 1u);
    EXPECT_EQ(true_value.data[0], '\1');
}

TEST(ValueCodecTest, VarUIntRoundTripsUnsignedIntegers) {
    Value value = valuecodec::make_varuint(300);
    std::uint64_t decoded = 0;

    ASSERT_EQ(value.type, ValueType::VarUInt);
    ASSERT_EQ(valuecodec::decode_varuint(value, &decoded), true);
    EXPECT_EQ(decoded, 300u);
}

TEST(ValueCodecTest, VarIntRoundTripsSignedIntegers) {
    Value value = valuecodec::make_varint(-300);
    std::int64_t decoded = 0;

    ASSERT_EQ(value.type, ValueType::VarInt);
    ASSERT_EQ(valuecodec::decode_varint(value, &decoded), true);
    EXPECT_EQ(decoded, -300);
}

} // namespace
