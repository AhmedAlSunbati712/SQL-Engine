#pragma once

#include <Value.h>
#include <string_view>
#include <vector>

namespace valuecodec {

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
