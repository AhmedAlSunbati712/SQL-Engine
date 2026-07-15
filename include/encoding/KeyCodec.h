#pragma once

#include <Key.h>
#include <cstdint>
#include <string_view>
#include <vector>

namespace keycodec {

std::int32_t compare(const Key &lhs, const Key &rhs);
bool equal(const Key &lhs, const Key &rhs);
bool validate_key(const Key &key);

Key make_bool(bool value);
Key make_uint64(std::uint64_t value);
Key make_int64(std::int64_t value);
Key make_string(std::string_view value);
Key make_bytes(const std::vector<char> &value);

} // namespace keycodec
