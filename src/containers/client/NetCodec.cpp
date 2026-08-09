#include <NetCodec.h>

#include <KeyCodec.h>

#include <arpa/inet.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace NetCodec {

namespace {

constexpr std::size_t TYPE_OFFSET = 0;
constexpr std::size_t RESERVED_OFFSET = 1;
constexpr std::size_t RESERVED_SIZE = 3;
constexpr std::size_t PAYLOAD_SIZE_OFFSET = 4;
constexpr std::size_t HEADER_SIZE = 8;

std::uint8_t serialize_key_type(KeyType type) {
    switch (type) {
        case KeyType::Bool:
            return 1;
        case KeyType::UInt64:
            return 2;
        case KeyType::Int64:
            return 3;
        case KeyType::String:
            return 4;
        case KeyType::Bytes:
            return 5;
    }

    throw std::invalid_argument("Unknown key type");
}

KeyType deserialize_key_type(std::uint8_t type) {
    switch (type) {
        case 1:
            return KeyType::Bool;
        case 2:
            return KeyType::UInt64;
        case 3:
            return KeyType::Int64;
        case 4:
            return KeyType::String;
        case 5:
            return KeyType::Bytes;
        default:
            throw std::runtime_error("Unknown network key type");
    }
}

void validate_reserved_bytes(const std::vector<std::uint8_t>& buffer) {
    for (std::size_t i = 0; i < RESERVED_SIZE; i++) {
        if (buffer[RESERVED_OFFSET + i] != 0) {
            throw std::runtime_error("Key reserved bytes must be zero");
        }
    }
}

} // namespace

std::vector<std::uint8_t> serialize_key(const Key& key) {
    if (!KeyCodec::validate_key(key)) {
        throw std::invalid_argument("Cannot serialize invalid key");
    }

    const std::uint32_t host_payload_size = key.size;
    const std::uint32_t network_payload_size = htonl(host_payload_size);

    std::vector<std::uint8_t> buffer(HEADER_SIZE + host_payload_size, 0);
    buffer[TYPE_OFFSET] = serialize_key_type(key.type);

    std::memcpy(
        buffer.data() + PAYLOAD_SIZE_OFFSET,
        &network_payload_size,
        sizeof(network_payload_size)
    );
    if (host_payload_size != 0) {
        std::memcpy(
            buffer.data() + HEADER_SIZE,
            key.data.data(),
            host_payload_size
        );
    }

    return buffer;
}

Key deserialize_key(const std::vector<std::uint8_t>& buffer) {
    if (buffer.size() < HEADER_SIZE) {
        throw std::runtime_error("Key buffer is smaller than its header");
    }

    validate_reserved_bytes(buffer);

    std::uint32_t network_payload_size = 0;
    std::memcpy(
        &network_payload_size,
        buffer.data() + PAYLOAD_SIZE_OFFSET,
        sizeof(network_payload_size)
    );
    const std::uint32_t host_payload_size = ntohl(network_payload_size);

    if (host_payload_size != buffer.size() - HEADER_SIZE) {
        throw std::runtime_error("Key payload size does not match buffer size");
    }

    Key key{};
    key.type = deserialize_key_type(buffer[TYPE_OFFSET]);
    key.size = host_payload_size;
    key.data.assign(
        reinterpret_cast<const char*>(buffer.data() + HEADER_SIZE),
        reinterpret_cast<const char*>(buffer.data() + buffer.size())
    );

    if (!KeyCodec::validate_key(key)) {
        throw std::runtime_error("Network bytes contain an invalid key");
    }

    return key;
}

} // namespace NetCodec
