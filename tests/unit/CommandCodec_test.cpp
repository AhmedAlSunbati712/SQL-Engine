#include <gtest/gtest.h>

#include <Command.h>
#include <KeyCodec.h>
#include <ValueCodec.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect_commands_equal(const Command& actual, const Command& expected) {
    EXPECT_EQ(actual.operator_type, expected.operator_type);
    EXPECT_EQ(actual.op, expected.op);
    ASSERT_EQ(actual.key.has_value(), expected.key.has_value());
    ASSERT_EQ(actual.value.has_value(), expected.value.has_value());

    if (actual.key.has_value()) {
        EXPECT_TRUE(KeyCodec::equal(*actual.key, *expected.key));
    }

    if (actual.value.has_value()) {
        EXPECT_TRUE(ValueCodec::equal(*actual.value, *expected.value));
    }
}

TEST(CommandCodecTest, SerializesNullaryCommandLayout) {
    const Command command{.operator_type = OperatorType::NULLARY, .op = Operator::BEGIN_TXN};
    const std::vector<std::uint8_t> encoded = CommandCodec::serialize(command);

    const std::vector<std::uint8_t> expected = {
        0, 0, 0, 2,
        static_cast<std::uint8_t>(OperatorType::NULLARY),
        static_cast<std::uint8_t>(Operator::BEGIN_TXN)
    };
    EXPECT_EQ(encoded, expected);
}

TEST(CommandCodecTest, SerializesUnaryCommandLayout) {
    const Command command{
        .operator_type = OperatorType::UNARY,
        .op = Operator::GET,
        .key = KeyCodec::make_bool(true)
    };
    const std::vector<std::uint8_t> encoded = CommandCodec::serialize(command);

    const std::vector<std::uint8_t> expected = {
        0, 0, 0, 12,
        static_cast<std::uint8_t>(OperatorType::UNARY),
        static_cast<std::uint8_t>(Operator::GET),
        0, 0, 0, 6,
        static_cast<std::uint8_t>(KeyType::Bool),
        0, 0, 0, 1,
        1
    };
    EXPECT_EQ(encoded, expected);
}

TEST(CommandCodecTest, SerializesBinaryCommandLayout) {
    const Command command{
        .operator_type = OperatorType::BINARY,
        .op = Operator::PUT,
        .key = KeyCodec::make_string("k"),
        .value = ValueCodec::make_char("v")
    };
    const std::vector<std::uint8_t> encoded = CommandCodec::serialize(command);

    const std::vector<std::uint8_t> expected = {
        0, 0, 0, 22,
        static_cast<std::uint8_t>(OperatorType::BINARY),
        static_cast<std::uint8_t>(Operator::PUT),
        0, 0, 0, 6,
        static_cast<std::uint8_t>(KeyType::String),
        0, 0, 0, 1,
        'k',
        0, 0, 0, 6,
        static_cast<std::uint8_t>(ValueType::Char),
        0, 0, 0, 1,
        'v'
    };
    EXPECT_EQ(encoded, expected);
}

TEST(CommandCodecTest, AllCommandKindsRoundTrip) {
    const std::vector<Command> commands = {
        Command{.operator_type = OperatorType::NULLARY, .op = Operator::BEGIN_TXN},
        Command{.operator_type = OperatorType::NULLARY, .op = Operator::COMMIT},
        Command{.operator_type = OperatorType::NULLARY, .op = Operator::ROLLBACK},
        Command{.operator_type = OperatorType::UNARY, .op = Operator::GET, .key = KeyCodec::make_uint64(42)},
        Command{.operator_type = OperatorType::UNARY, .op = Operator::DELETE, .key = KeyCodec::make_string("key")},
        Command{
            .operator_type = OperatorType::BINARY,
            .op = Operator::PUT,
            .key = KeyCodec::make_bytes(std::vector<char>{'k', '\0'}),
            .value = ValueCodec::make_char(std::string{"v\0", 2})
        }
    };

    for (const Command& command : commands) {
        const Command decoded = CommandCodec::deserialize(CommandCodec::serialize(command));
        expect_commands_equal(decoded, command);
    }
}

TEST(CommandCodecTest, SerializeRejectsInvalidCommandShapes) {
    const Command wrong_nullary_op{.operator_type = OperatorType::NULLARY, .op = Operator::GET};
    EXPECT_THROW(CommandCodec::serialize(wrong_nullary_op), std::invalid_argument);

    const Command missing_key{.operator_type = OperatorType::UNARY, .op = Operator::GET};
    EXPECT_THROW(CommandCodec::serialize(missing_key), std::invalid_argument);

    const Command extra_value{
        .operator_type = OperatorType::UNARY,
        .op = Operator::GET,
        .key = KeyCodec::make_bool(true),
        .value = ValueCodec::make_bool(true)
    };
    EXPECT_THROW(CommandCodec::serialize(extra_value), std::invalid_argument);

    const Command missing_value{
        .operator_type = OperatorType::BINARY,
        .op = Operator::PUT,
        .key = KeyCodec::make_bool(true)
    };
    EXPECT_THROW(CommandCodec::serialize(missing_value), std::invalid_argument);

    const Command wrong_binary_op{
        .operator_type = OperatorType::BINARY,
        .op = Operator::DELETE,
        .key = KeyCodec::make_bool(true),
        .value = ValueCodec::make_bool(true)
    };
    EXPECT_THROW(CommandCodec::serialize(wrong_binary_op), std::invalid_argument);
}

TEST(CommandCodecTest, DeserializeRejectsInvalidHeadersAndOperators) {
    EXPECT_THROW(CommandCodec::deserialize(std::vector<std::uint8_t>(5, 0)), std::runtime_error);

    const std::vector<std::uint8_t> wrong_payload_size = {
        0, 0, 0, 3,
        static_cast<std::uint8_t>(OperatorType::NULLARY),
        static_cast<std::uint8_t>(Operator::BEGIN_TXN)
    };
    EXPECT_THROW(CommandCodec::deserialize(wrong_payload_size), std::runtime_error);

    const std::vector<std::uint8_t> unknown_type = {0, 0, 0, 2, 99, 0};
    EXPECT_THROW(CommandCodec::deserialize(unknown_type), std::runtime_error);

    const std::vector<std::uint8_t> unknown_operator = {0, 0, 0, 2, 0, 99};
    EXPECT_THROW(CommandCodec::deserialize(unknown_operator), std::runtime_error);

    const std::vector<std::uint8_t> wrong_operator_kind = {
        0, 0, 0, 2,
        static_cast<std::uint8_t>(OperatorType::UNARY),
        static_cast<std::uint8_t>(Operator::PUT)
    };
    EXPECT_THROW(CommandCodec::deserialize(wrong_operator_kind), std::runtime_error);
}

TEST(CommandCodecTest, DeserializeRejectsMalformedOperands) {
    const std::vector<std::uint8_t> oversized_key = {
        0, 0, 0, 6,
        static_cast<std::uint8_t>(OperatorType::UNARY),
        static_cast<std::uint8_t>(Operator::GET),
        0, 0, 0, 6
    };
    EXPECT_THROW(CommandCodec::deserialize(oversized_key), std::runtime_error);

    const std::vector<std::uint8_t> malformed_key = {
        0, 0, 0, 11,
        static_cast<std::uint8_t>(OperatorType::UNARY),
        static_cast<std::uint8_t>(Operator::GET),
        0, 0, 0, 5,
        99, 0, 0, 0, 0
    };
    EXPECT_THROW(CommandCodec::deserialize(malformed_key), std::runtime_error);

    const std::vector<std::uint8_t> missing_value_size = {
        0, 0, 0, 12,
        static_cast<std::uint8_t>(OperatorType::BINARY),
        static_cast<std::uint8_t>(Operator::PUT),
        0, 0, 0, 6,
        static_cast<std::uint8_t>(KeyType::Bool),
        0, 0, 0, 1,
        1
    };
    EXPECT_THROW(CommandCodec::deserialize(missing_value_size), std::runtime_error);
}

} // namespace
