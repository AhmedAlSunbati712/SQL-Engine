#include <Session.h>

#include <NetCodec.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t MAX_VALUE_RESPONSE_SIZE = static_cast<std::uint32_t>(ValueCodec::MAX_PAYLOAD_SIZE + 5);

void send_all(int fd, const std::vector<std::uint8_t> &buffer) {
    std::size_t bytes_sent = 0;

    while (bytes_sent < buffer.size()) {
        const ssize_t result = ::send(fd, buffer.data() + bytes_sent, buffer.size() - bytes_sent, MSG_NOSIGNAL);
        if (result > 0) {
            bytes_sent += static_cast<std::size_t>(result);
            continue;
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        throw std::runtime_error("[ERROR] Failed to send command");
    }
}

void receive_all(int fd, std::uint8_t *buffer, std::size_t size) {
    std::size_t bytes_received = 0;

    while (bytes_received < size) {
        const ssize_t result = ::recv(fd, buffer + bytes_received, size - bytes_received, 0);
        if (result > 0) {
            bytes_received += static_cast<std::size_t>(result);
            continue;
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result == 0) {
            throw std::runtime_error("[ERROR] Server closed the connection");
        }

        throw std::runtime_error("[ERROR] Failed to receive response");
    }
}

Key encode_key(const KeyInput &key) {
    const std::optional<Key> encoded_key = KeyCodec::encode(key);
    if (!encoded_key.has_value()) {
        throw std::invalid_argument("[ERROR] Invalid key");
    }

    return *encoded_key;
}

Value encode_value(const ValueInput &value) {
    const std::optional<Value> encoded_value = ValueCodec::encode(value);
    if (!encoded_value.has_value()) {
        throw std::invalid_argument("[ERROR] Invalid value");
    }

    return *encoded_value;
}

} // namespace

Session::Session(int fd_, int id_): fd(fd_), id(id_) {
    if (fd < 0) {
        throw std::invalid_argument("[ERROR] Invalid session socket");
    }
}

Session::~Session() {
    close();
}

Session::Session(Session &&other) noexcept: fd(other.fd), id(other.id) {
    other.fd = -1;
    other.id = -1;
}

Session &Session::operator=(Session &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    close();
    fd = other.fd;
    id = other.id;
    other.fd = -1;
    other.id = -1;
    return *this;
}

int Session::get_id() const {
    return id;
}

std::optional<ValueInput> Session::get(const KeyInput &key) {
    const Command command{.operator_type = OperatorType::UNARY, .op = Operator::GET, .key = encode_key(key)};
    send_command(command);
    return read_get_response();
}

void Session::put(const KeyInput &key, const ValueInput &value) {
    const Command command{.operator_type = OperatorType::BINARY, .op = Operator::PUT, .key = encode_key(key), .value = encode_value(value)};
    send_command(command);
}

void Session::remove(const KeyInput &key) {
    const Command command{.operator_type = OperatorType::UNARY, .op = Operator::DELETE, .key = encode_key(key)};
    send_command(command);
}

void Session::begin_transaction() {
    send_command(Command{.operator_type = OperatorType::NULLARY, .op = Operator::BEGIN_TXN});
}

void Session::commit() {
    send_command(Command{.operator_type = OperatorType::NULLARY, .op = Operator::COMMIT});
}

void Session::rollback() {
    send_command(Command{.operator_type = OperatorType::NULLARY, .op = Operator::ROLLBACK});
}

void Session::close() noexcept {
    if (fd < 0) {
        return;
    }

    ::close(fd);
    fd = -1;
}

void Session::send_command(const Command &command) {
    if (fd < 0) {
        throw std::runtime_error("[ERROR] Session is closed");
    }

    try {
        send_all(fd, CommandCodec::serialize(command));
    } catch (...) {
        close();
        throw;
    }
}

std::optional<ValueInput> Session::read_get_response() {
    if (fd < 0) {
        throw std::runtime_error("[ERROR] Session is closed");
    }

    try {
        std::uint32_t network_response_size = 0;
        receive_all(fd, reinterpret_cast<std::uint8_t *>(&network_response_size), sizeof(network_response_size));
        const std::uint32_t host_response_size = ntohl(network_response_size);

        if (host_response_size == 0) {
            return std::nullopt;
        }

        if (host_response_size > MAX_VALUE_RESPONSE_SIZE) {
            throw std::runtime_error("[ERROR] Value response is too large");
        }

        std::vector<std::uint8_t> network_value(host_response_size);
        receive_all(fd, network_value.data(), network_value.size());

        const Value value = NetCodec::deserialize_value(network_value);
        const std::optional<ValueInput> decoded_value = ValueCodec::decode(value);
        if (!decoded_value.has_value()) {
            throw std::runtime_error("[ERROR] Failed to decode value response");
        }

        return decoded_value;
    } catch (...) {
        close();
        throw;
    }
}
