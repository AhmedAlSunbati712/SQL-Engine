#include <NetCodec.h>

#include <KeyCodec.h>
#include <ValueCodec.h>

#include <arpa/inet.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace NetCodec {

namespace {

constexpr std::size_t TYPE_OFFSET = 0;
constexpr std::size_t PAYLOAD_SIZE_OFFSET = 1;
constexpr std::size_t HEADER_SIZE = 5;

static_assert(sizeof(KeyType) == sizeof(std::uint8_t));
static_assert(sizeof(ValueType) == sizeof(std::uint8_t));

KeyType deserialize_key_type(std::uint8_t type) {
    switch (static_cast<KeyType>(type)) {
        case KeyType::Bool:
            return KeyType::Bool;
        case KeyType::UInt64:
            return KeyType::UInt64;
        case KeyType::Int64:
            return KeyType::Int64;
        case KeyType::String:
            return KeyType::String;
        case KeyType::Bytes:
            return KeyType::Bytes;
    }

    throw std::runtime_error("Unknown network key type");
}

ValueType deserialize_value_type(std::uint8_t type) {
    switch (static_cast<ValueType>(type)) {
        case ValueType::VarUInt:
            return ValueType::VarUInt;
        case ValueType::VarInt:
            return ValueType::VarInt;
        case ValueType::Bool:
            return ValueType::Bool;
        case ValueType::Char:
            return ValueType::Char;
    }

    throw std::runtime_error("Unknown network value type");
}

} // namespace

std::vector<std::uint8_t> serialize_key(const Key& key) {
    if (!KeyCodec::validate_key(key)) {
        throw std::invalid_argument("Cannot serialize invalid key");
    }

    const std::uint32_t host_payload_size = key.size;
    const std::uint32_t network_payload_size = htonl(host_payload_size);

    std::vector<std::uint8_t> buffer(HEADER_SIZE + host_payload_size);
    buffer[TYPE_OFFSET] = static_cast<std::uint8_t>(key.type);
    std::memcpy(buffer.data() + PAYLOAD_SIZE_OFFSET, &network_payload_size, sizeof(network_payload_size));

    if (host_payload_size != 0) {
        std::memcpy(buffer.data() + HEADER_SIZE, key.data.data(), host_payload_size);
    }

    return buffer;
}

Key deserialize_key(const std::vector<std::uint8_t>& buffer) {
    if (buffer.size() < HEADER_SIZE) {
        throw std::runtime_error("Key buffer is smaller than its header");
    }

    std::uint32_t network_payload_size = 0;
    std::memcpy(&network_payload_size, buffer.data() + PAYLOAD_SIZE_OFFSET, sizeof(network_payload_size));
    const std::uint32_t host_payload_size = ntohl(network_payload_size);

    if (host_payload_size != buffer.size() - HEADER_SIZE) {
        throw std::runtime_error("Key payload size does not match buffer size");
    }

    Key key{};
    key.type = deserialize_key_type(buffer[TYPE_OFFSET]);
    key.size = host_payload_size;
    key.data.resize(host_payload_size);

    if (host_payload_size != 0) {
        std::memcpy(key.data.data(), buffer.data() + HEADER_SIZE, host_payload_size);
    }

    if (!KeyCodec::validate_key(key)) {
        throw std::runtime_error("Network bytes contain an invalid key");
    }

    return key;
}

std::vector<std::uint8_t> serialize_value(const Value& value) {
    if (!ValueCodec::validate_value(value)) {
        throw std::invalid_argument("Cannot serialize invalid value");
    }

    const std::uint32_t host_payload_size = value.size;
    const std::uint32_t network_payload_size = htonl(host_payload_size);

    std::vector<std::uint8_t> buffer(HEADER_SIZE + host_payload_size);
    buffer[TYPE_OFFSET] = static_cast<std::uint8_t>(value.type);
    std::memcpy(buffer.data() + PAYLOAD_SIZE_OFFSET, &network_payload_size, sizeof(network_payload_size));

    if (host_payload_size != 0) {
        std::memcpy(buffer.data() + HEADER_SIZE, value.data.data(), host_payload_size);
    }

    return buffer;
}

Value deserialize_value(const std::vector<std::uint8_t>& buffer) {
    if (buffer.size() < HEADER_SIZE) {
        throw std::runtime_error("Value buffer is smaller than its header");
    }

    std::uint32_t network_payload_size = 0;
    std::memcpy(&network_payload_size, buffer.data() + PAYLOAD_SIZE_OFFSET, sizeof(network_payload_size));
    const std::uint32_t host_payload_size = ntohl(network_payload_size);

    if (host_payload_size != buffer.size() - HEADER_SIZE) {
        throw std::runtime_error("Value payload size does not match buffer size");
    }

    Value value{};
    value.type = deserialize_value_type(buffer[TYPE_OFFSET]);
    value.size = host_payload_size;
    value.data.resize(host_payload_size);

    if (host_payload_size != 0) {
        std::memcpy(value.data.data(), buffer.data() + HEADER_SIZE, host_payload_size);
    }

    if (!ValueCodec::validate_value(value)) {
        throw std::runtime_error("Network bytes contain an invalid value");
    }

    return value;
}

} // namespace NetCodec
