#pragma once

#include <Key.h>
#include <Value.h>

#include <cstdint>
#include <vector>

namespace NetCodec {

std::vector<std::uint8_t> serialize_key(const Key& key);
Key deserialize_key(const std::vector<std::uint8_t>& buffer);

std::vector<std::uint8_t> serialize_value(const Value& value);
Value deserialize_value(const std::vector<std::uint8_t>& buffer);

} // namespace NetCodec
