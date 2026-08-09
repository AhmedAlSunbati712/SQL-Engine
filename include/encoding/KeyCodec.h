#pragma once

#include <Key.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using KeyInput = std::variant<
    bool,
    std::uint64_t,
    std::int64_t,
    std::string,
    std::vector<char>
>;

namespace KeyCodec {

inline constexpr std::size_t MAX_PAYLOAD_SIZE =
    std::numeric_limits<std::uint16_t>::max();

std::optional<Key> encode(const KeyInput &input);
std::optional<KeyInput> decode(const Key &key);

std::int32_t compare(const Key &lhs, const Key &rhs);
bool equal(const Key &lhs, const Key &rhs);
bool validate_key(const Key &key);

Key make_bool(bool value);
Key make_uint64(std::uint64_t value);
Key make_int64(std::int64_t value);
Key make_string(std::string_view value);
Key make_bytes(const std::vector<char> &value);

} // namespace KeyCodec
