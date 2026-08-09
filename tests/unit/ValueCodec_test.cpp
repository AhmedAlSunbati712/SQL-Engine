#include <gtest/gtest.h>

#include <ValueCodec.h>

#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

TEST(ValueCodecTest, EqualRequiresSameTypeSizeAndPayload) {
    Value lhs = ValueCodec::make_char("cat");
    Value rhs = ValueCodec::make_char("cat");
    Value different_type = ValueCodec::make_varuint(7);
    Value different_payload = ValueCodec::make_char("dog");

    EXPECT_EQ(ValueCodec::equal(lhs, rhs), true);
    EXPECT_EQ(ValueCodec::equal(lhs, different_type), false);
    EXPECT_EQ(ValueCodec::equal(lhs, different_payload), false);
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

    EXPECT_EQ(ValueCodec::validate_value(bad_varuint), false);
    EXPECT_EQ(ValueCodec::validate_value(bad_bool), false);
    EXPECT_EQ(ValueCodec::validate_value(bad_varint), false);
    EXPECT_EQ(ValueCodec::validate_value(bad_size), false);
}

TEST(ValueCodecTest, ValidateValueAcceptsFactoryOutput) {
    EXPECT_EQ(ValueCodec::validate_value(ValueCodec::make_varuint(123)), true);
    EXPECT_EQ(ValueCodec::validate_value(ValueCodec::make_varint(-123)), true);
    EXPECT_EQ(ValueCodec::validate_value(ValueCodec::make_bool(true)), true);
    EXPECT_EQ(ValueCodec::validate_value(ValueCodec::make_char("hello")), true);
}

TEST(ValueCodecTest, BoolFactoryEncodesSingleBytePayload) {
    Value false_value = ValueCodec::make_bool(false);
    Value true_value = ValueCodec::make_bool(true);

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
    Value value = ValueCodec::make_varuint(300);
    std::uint64_t decoded = 0;

    ASSERT_EQ(value.type, ValueType::VarUInt);
    ASSERT_EQ(ValueCodec::decode_varuint(value, &decoded), true);
    EXPECT_EQ(decoded, 300u);
}

TEST(ValueCodecTest, VarIntRoundTripsSignedIntegers) {
    Value value = ValueCodec::make_varint(-300);
    std::int64_t decoded = 0;

    ASSERT_EQ(value.type, ValueType::VarInt);
    ASSERT_EQ(ValueCodec::decode_varint(value, &decoded), true);
    EXPECT_EQ(decoded, -300);
}

TEST(ValueCodecTest, PrimitiveInputsRoundTripThroughCanonicalEncoding) {
    const std::vector<ValueInput> inputs = {
        ValueInput{false},
        ValueInput{std::numeric_limits<std::uint64_t>::max()},
        ValueInput{std::numeric_limits<std::int64_t>::min()},
        ValueInput{std::string{"value\0text", 10}}
    };

    for (const ValueInput &input : inputs) {
        std::optional<Value> encoded = ValueCodec::encode(input);
        ASSERT_TRUE(encoded.has_value());
        EXPECT_TRUE(ValueCodec::validate_value(*encoded));

        std::optional<ValueInput> decoded = ValueCodec::decode(*encoded);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, input);
    }
}

TEST(ValueCodecTest, RejectsOversizedPayloadAndNonCanonicalBool) {
    const std::string oversized(ValueCodec::MAX_PAYLOAD_SIZE + 1, 'x');
    EXPECT_FALSE(ValueCodec::encode(ValueInput{oversized}).has_value());
    EXPECT_FALSE(ValueCodec::validate_value(ValueCodec::make_char(oversized)));

    Value malformed_bool{};
    malformed_bool.type = ValueType::Bool;
    malformed_bool.size = 1;
    malformed_bool.data = {'x'};
    EXPECT_FALSE(ValueCodec::validate_value(malformed_bool));
    EXPECT_FALSE(ValueCodec::decode(malformed_bool).has_value());
}

} // namespace
