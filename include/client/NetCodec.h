#pragma once

#include <Key.h>

#include <cstdint>
#include <vector>

namespace NetCodec {

std::vector<std::uint8_t> serialize_key(const Key& key);
Key deserialize_key(const std::vector<std::uint8_t>& buffer);

} // namespace NetCodec
