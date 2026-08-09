#include <KeyCodec.h>

#include <Endian.h>

#include <array>
#include <bit>
#include <cstring>
#include <type_traits>

namespace KeyCodec {

namespace {

std::int32_t compare_bytes(const std::vector<char> &lhs, const std::vector<char> &rhs) {
    const std::size_t min_size = (lhs.size() < rhs.size()) ? lhs.size() : rhs.size();
    for (std::size_t i = 0; i < min_size; i++) {
        const unsigned char lhs_byte = static_cast<unsigned char>(lhs[i]);
        const unsigned char rhs_byte = static_cast<unsigned char>(rhs[i]);
        if (lhs_byte < rhs_byte) return -1;
        if (lhs_byte > rhs_byte) return 1;
    }

    if (lhs.size() < rhs.size()) return -1;
    if (lhs.size() > rhs.size()) return 1;
    return 0;
}

} // namespace

std::optional<Key> encode(const KeyInput &input) {
    return std::visit(
        [](const auto &value) -> std::optional<Key> {
            using T = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<T, bool>) {
                return make_bool(value);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                return make_uint64(value);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return make_int64(value);
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (value.size() > MAX_PAYLOAD_SIZE) return std::nullopt;
                return make_string(value);
            } else {
                if (value.size() > MAX_PAYLOAD_SIZE) return std::nullopt;
                return make_bytes(value);
            }
        },
        input
    );
}

std::optional<KeyInput> decode(const Key &key) {
    if (!validate_key(key)) return std::nullopt;

    switch (key.type) {
        case KeyType::Bool:
            return KeyInput{key.data[0] == '\1'};
        case KeyType::UInt64:
            return KeyInput{get_u64_be(key.data.data())};
        case KeyType::Int64: {
            const std::uint64_t encoded = get_u64_be(key.data.data());
            const std::uint64_t raw = encoded ^ (1ULL << 63);
            return KeyInput{std::bit_cast<std::int64_t>(raw)};
        }
        case KeyType::String:
            return KeyInput{std::string(key.data.begin(), key.data.end())};
        case KeyType::Bytes:
            return KeyInput{key.data};
    }

    return std::nullopt;
}

std::int32_t compare(const Key &lhs, const Key &rhs) {
    if (lhs.type != rhs.type) {
        return (static_cast<std::uint8_t>(lhs.type) < static_cast<std::uint8_t>(rhs.type)) ? -1 : 1;
    }

    return compare_bytes(lhs.data, rhs.data);
}

bool equal(const Key &lhs, const Key &rhs) {
    return compare(lhs, rhs) == 0;
}

bool validate_key(const Key &key) {
    if (key.size != key.data.size()) return false;
    if (key.size > MAX_PAYLOAD_SIZE) return false;

    switch (key.type) {
        case KeyType::Bool:
            return key.size == 1 && (key.data[0] == '\0' || key.data[0] == '\1');
        case KeyType::UInt64:
            return key.size == 8;
        case KeyType::Int64:
            return key.size == 8;
        case KeyType::String:
            return true;
        case KeyType::Bytes:
            return true;
    }

    return false;
}

Key make_bool(bool value) {
    Key key{};
    key.type = KeyType::Bool;
    key.size = 1;
    key.data.push_back(value ? '\1' : '\0');
    return key;
}

Key make_uint64(std::uint64_t value) {
    Key key{};
    key.type = KeyType::UInt64;
    key.size = 8;
    key.data.resize(8);
    put_u64_be(key.data.data(), value);
    return key;
}

Key make_int64(std::int64_t value) {
    Key key{};
    key.type = KeyType::Int64;
    key.size = 8;
    key.data.resize(8);

    std::uint64_t biased = static_cast<std::uint64_t>(value) ^ (1ULL << 63);
    put_u64_be(key.data.data(), biased);
    return key;
}

Key make_string(std::string_view value) {
    Key key{};
    key.type = KeyType::String;
    key.size = static_cast<std::uint32_t>(value.size());
    key.data.assign(value.begin(), value.end());
    return key;
}

Key make_bytes(const std::vector<char> &value) {
    Key key{};
    key.type = KeyType::Bytes;
    key.size = static_cast<std::uint32_t>(value.size());
    key.data = value;
    return key;
}

} // namespace KeyCodec
