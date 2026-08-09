#pragma once

#include <Value.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using ValueInput = std::variant<
    bool,
    std::uint64_t,
    std::int64_t,
    std::string
>;

namespace valuecodec {

inline constexpr std::size_t MAX_PAYLOAD_SIZE =
    std::numeric_limits<std::uint16_t>::max();

std::optional<Value> encode(const ValueInput &input);
std::optional<ValueInput> decode(const Value &value);

bool equal(const Value &lhs, const Value &rhs);
bool validate_value(const Value &value);

// Encode an unsigned integer into the on-disk VarUInt representation.
Value make_varuint(std::uint64_t value);

// Encode a signed integer by first ZigZag-transforming it, then
// serializing the result using the unsigned varint format.
Value make_varint(std::int64_t value);

Value make_bool(bool value);
Value make_char(std::string_view value);

// Decode helpers for higher-level modules that want to work with
// primitive integers instead of raw byte vectors.
bool decode_varuint(const Value &value, std::uint64_t *out);
bool decode_varint(const Value &value, std::int64_t *out);

} // namespace valuecodec
