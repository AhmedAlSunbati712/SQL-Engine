#include <Command.h>

#include <NetCodec.h>

#include <arpa/inet.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace CommandCodec {

namespace {

constexpr std::size_t PAYLOAD_SIZE_BYTES = sizeof(std::uint32_t);
constexpr std::size_t OPERATOR_BYTES = sizeof(std::uint8_t) * 2;
constexpr std::size_t COMMAND_HEADER_BYTES = PAYLOAD_SIZE_BYTES + OPERATOR_BYTES;

static_assert(sizeof(OperatorType) == sizeof(std::uint8_t));
static_assert(sizeof(Operator) == sizeof(std::uint8_t));

bool is_nullary_operator(Operator op) {
    return op == Operator::BEGIN_TXN || op == Operator::COMMIT || op == Operator::ROLLBACK;
}

bool is_unary_operator(Operator op) {
    return op == Operator::GET || op == Operator::DELETE;
}

std::uint32_t checked_size(std::size_t size, const char* field_name) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(field_name);
    }

    return static_cast<std::uint32_t>(size);
}

std::uint8_t read_byte(const std::vector<std::uint8_t>& buffer, std::size_t& bytes_offset) {
    if (bytes_offset >= buffer.size()) {
        throw std::runtime_error("Command buffer ended before the next byte");
    }

    return buffer[bytes_offset++];
}

std::uint32_t read_size(const std::vector<std::uint8_t>& buffer, std::size_t& bytes_offset) {
    if (bytes_offset > buffer.size() || buffer.size() - bytes_offset < sizeof(std::uint32_t)) {
        throw std::runtime_error("Command buffer ended before the next size field");
    }

    std::uint32_t network_size = 0;
    std::memcpy(&network_size, buffer.data() + bytes_offset, sizeof(network_size));
    bytes_offset += sizeof(network_size);
    return ntohl(network_size);
}

std::vector<std::uint8_t> read_bytes(const std::vector<std::uint8_t>& buffer, std::size_t& bytes_offset, std::uint32_t size) {
    if (bytes_offset > buffer.size() || size > buffer.size() - bytes_offset) {
        throw std::runtime_error("Command operand size exceeds the remaining buffer");
    }

    const auto begin = buffer.begin() + static_cast<std::ptrdiff_t>(bytes_offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(size);
    bytes_offset += size;
    return std::vector<std::uint8_t>(begin, end);
}

OperatorType read_operator_type(const std::vector<std::uint8_t>& buffer, std::size_t& bytes_offset) {
    const std::uint8_t type = read_byte(buffer, bytes_offset);

    switch (static_cast<OperatorType>(type)) {
        case OperatorType::NULLARY:
            return OperatorType::NULLARY;
        case OperatorType::UNARY:
            return OperatorType::UNARY;
        case OperatorType::BINARY:
            return OperatorType::BINARY;
    }

    throw std::runtime_error("Unknown command operator type");
}

Operator read_operator(const std::vector<std::uint8_t>& buffer, std::size_t& bytes_offset) {
    const std::uint8_t op = read_byte(buffer, bytes_offset);

    switch (static_cast<Operator>(op)) {
        case Operator::GET:
            return Operator::GET;
        case Operator::PUT:
            return Operator::PUT;
        case Operator::DELETE:
            return Operator::DELETE;
        case Operator::BEGIN_TXN:
            return Operator::BEGIN_TXN;
        case Operator::COMMIT:
            return Operator::COMMIT;
        case Operator::ROLLBACK:
            return Operator::ROLLBACK;
    }

    throw std::runtime_error("Unknown command operator");
}

void write_command_header(std::vector<std::uint8_t>& buffer, std::uint32_t host_payload_size, const Command& command, std::size_t& bytes_offset) {
    const std::uint32_t network_payload_size = htonl(host_payload_size);
    std::memcpy(buffer.data() + bytes_offset, &network_payload_size, sizeof(network_payload_size));
    bytes_offset += sizeof(network_payload_size);
    buffer[bytes_offset++] = static_cast<std::uint8_t>(command.operator_type);
    buffer[bytes_offset++] = static_cast<std::uint8_t>(command.op);
}

void write_size(std::vector<std::uint8_t>& buffer, std::uint32_t host_size, std::size_t& bytes_offset) {
    const std::uint32_t network_size = htonl(host_size);
    std::memcpy(buffer.data() + bytes_offset, &network_size, sizeof(network_size));
    bytes_offset += sizeof(network_size);
}

void write_bytes(std::vector<std::uint8_t>& buffer, const std::vector<std::uint8_t>& bytes, std::size_t& bytes_offset) {
    if (!bytes.empty()) {
        std::memcpy(buffer.data() + bytes_offset, bytes.data(), bytes.size());
    }
    bytes_offset += bytes.size();
}

std::vector<std::uint8_t> serialize_nullary(const Command& command) {
    if (!is_nullary_operator(command.op) || command.key.has_value() || command.value.has_value()) {
        throw std::invalid_argument("Invalid nullary command");
    }

    const std::uint32_t host_payload_size = static_cast<std::uint32_t>(OPERATOR_BYTES);
    std::vector<std::uint8_t> buffer(PAYLOAD_SIZE_BYTES + host_payload_size);

    std::size_t bytes_offset = 0;
    write_command_header(buffer, host_payload_size, command, bytes_offset);
    return buffer;
}

std::vector<std::uint8_t> serialize_unary(const Command& command) {
    if (!is_unary_operator(command.op) || !command.key.has_value() || command.value.has_value()) {
        throw std::invalid_argument("Invalid unary command");
    }

    const std::vector<std::uint8_t> network_key = NetCodec::serialize_key(*command.key);
    const std::uint32_t host_key_size = checked_size(network_key.size(), "Serialized key is too large");
    const std::uint32_t host_payload_size = checked_size(OPERATOR_BYTES + sizeof(host_key_size) + network_key.size(), "Unary command payload is too large");
    std::vector<std::uint8_t> buffer(PAYLOAD_SIZE_BYTES + host_payload_size);

    std::size_t bytes_offset = 0;
    write_command_header(buffer, host_payload_size, command, bytes_offset);
    write_size(buffer, host_key_size, bytes_offset);
    write_bytes(buffer, network_key, bytes_offset);
    return buffer;
}

std::vector<std::uint8_t> serialize_binary(const Command& command) {
    if (command.op != Operator::PUT || !command.key.has_value() || !command.value.has_value()) {
        throw std::invalid_argument("Invalid binary command");
    }

    const std::vector<std::uint8_t> network_key = NetCodec::serialize_key(*command.key);
    const std::vector<std::uint8_t> network_value = NetCodec::serialize_value(*command.value);
    const std::uint32_t host_key_size = checked_size(network_key.size(), "Serialized key is too large");
    const std::uint32_t host_value_size = checked_size(network_value.size(), "Serialized value is too large");
    const std::size_t operand_bytes = sizeof(host_key_size) + network_key.size() + sizeof(host_value_size) + network_value.size();
    const std::uint32_t host_payload_size = checked_size(OPERATOR_BYTES + operand_bytes, "Binary command payload is too large");
    std::vector<std::uint8_t> buffer(PAYLOAD_SIZE_BYTES + host_payload_size);

    std::size_t bytes_offset = 0;
    write_command_header(buffer, host_payload_size, command, bytes_offset);
    write_size(buffer, host_key_size, bytes_offset);
    write_bytes(buffer, network_key, bytes_offset);
    write_size(buffer, host_value_size, bytes_offset);
    write_bytes(buffer, network_value, bytes_offset);
    return buffer;
}

Command deserialize_nullary(const std::vector<std::uint8_t>& buffer, std::size_t bytes_offset, Operator op) {
    if (!is_nullary_operator(op) || bytes_offset != buffer.size()) {
        throw std::runtime_error("Invalid nullary command packet");
    }

    return Command{.operator_type = OperatorType::NULLARY, .op = op};
}

Command deserialize_unary(const std::vector<std::uint8_t>& buffer, std::size_t bytes_offset, Operator op) {
    if (!is_unary_operator(op)) {
        throw std::runtime_error("Invalid unary command operator");
    }

    const std::uint32_t host_key_size = read_size(buffer, bytes_offset);
    const std::vector<std::uint8_t> network_key = read_bytes(buffer, bytes_offset, host_key_size);

    if (bytes_offset != buffer.size()) {
        throw std::runtime_error("Unary command packet has trailing bytes");
    }

    return Command{.operator_type = OperatorType::UNARY, .op = op, .key = NetCodec::deserialize_key(network_key)};
}

Command deserialize_binary(const std::vector<std::uint8_t>& buffer, std::size_t bytes_offset, Operator op) {
    if (op != Operator::PUT) {
        throw std::runtime_error("Invalid binary command operator");
    }

    const std::uint32_t host_key_size = read_size(buffer, bytes_offset);
    const std::vector<std::uint8_t> network_key = read_bytes(buffer, bytes_offset, host_key_size);
    const std::uint32_t host_value_size = read_size(buffer, bytes_offset);
    const std::vector<std::uint8_t> network_value = read_bytes(buffer, bytes_offset, host_value_size);

    if (bytes_offset != buffer.size()) {
        throw std::runtime_error("Binary command packet has trailing bytes");
    }

    return Command{
        .operator_type = OperatorType::BINARY,
        .op = op,
        .key = NetCodec::deserialize_key(network_key),
        .value = NetCodec::deserialize_value(network_value)
    };
}

} // namespace

std::vector<std::uint8_t> serialize(const Command& command) {
    switch (command.operator_type) {
        case OperatorType::NULLARY:
            return serialize_nullary(command);
        case OperatorType::UNARY:
            return serialize_unary(command);
        case OperatorType::BINARY:
            return serialize_binary(command);
    }

    throw std::invalid_argument("Unknown command operator type");
}

Command deserialize(const std::vector<std::uint8_t>& buffer) {
    if (buffer.size() < COMMAND_HEADER_BYTES) {
        throw std::runtime_error("Command buffer is smaller than its header");
    }

    std::size_t bytes_offset = 0;
    const std::uint32_t host_payload_size = read_size(buffer, bytes_offset);

    if (host_payload_size != buffer.size() - PAYLOAD_SIZE_BYTES) {
        throw std::runtime_error("Command payload size does not match buffer size");
    }

    const OperatorType operator_type = read_operator_type(buffer, bytes_offset);
    const Operator op = read_operator(buffer, bytes_offset);

    switch (operator_type) {
        case OperatorType::NULLARY:
            return deserialize_nullary(buffer, bytes_offset, op);
        case OperatorType::UNARY:
            return deserialize_unary(buffer, bytes_offset, op);
        case OperatorType::BINARY:
            return deserialize_binary(buffer, bytes_offset, op);
    }

    throw std::runtime_error("Unknown command operator type");
}

} // namespace CommandCodec
