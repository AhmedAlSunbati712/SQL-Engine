#include <server/CommandServer.h>

#include <Command.h>
#include <NetCodec.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace CommandServer {

namespace {

constexpr std::size_t PAYLOAD_SIZE_BYTES = sizeof(std::uint32_t);
constexpr std::uint32_t MAX_COMMAND_PAYLOAD_SIZE = 16 * 1024 * 1024;

void receive_all(int socket_fd, std::uint8_t *buffer, std::size_t size) {
    std::size_t bytes_received = 0;

    while (bytes_received < size) {
        const ssize_t result = ::recv(socket_fd, buffer + bytes_received, size - bytes_received, 0);
        if (result > 0) {
            bytes_received += static_cast<std::size_t>(result);
            continue;
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result == 0) {
            throw std::runtime_error("[ERROR] Client closed the connection");
        }

        throw std::runtime_error("[ERROR] Failed to receive command");
    }
}

void send_all(int socket_fd, const std::uint8_t *buffer, std::size_t size) {
    std::size_t bytes_sent = 0;

    while (bytes_sent < size) {
        const ssize_t result = ::send(socket_fd, buffer + bytes_sent, size - bytes_sent, MSG_NOSIGNAL);
        if (result > 0) {
            bytes_sent += static_cast<std::size_t>(result);
            continue;
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        throw std::runtime_error("[ERROR] Failed to send command response");
    }
}

Command read_command(int socket_fd) {
    // CommandCodec validates the complete packet, so retain the four-byte size
    // prefix while reading the payload in a separate exact-read operation.
    std::vector<std::uint8_t> packet(PAYLOAD_SIZE_BYTES);
    receive_all(socket_fd, packet.data(), packet.size());

    std::uint32_t network_payload_size = 0;
    std::memcpy(&network_payload_size, packet.data(), sizeof(network_payload_size));
    const std::uint32_t host_payload_size = ntohl(network_payload_size);
    if (host_payload_size > MAX_COMMAND_PAYLOAD_SIZE) {
        throw std::runtime_error("[ERROR] Command payload is too large");
    }

    packet.resize(PAYLOAD_SIZE_BYTES + static_cast<std::size_t>(host_payload_size));
    receive_all(socket_fd, packet.data() + PAYLOAD_SIZE_BYTES, host_payload_size);
    return CommandCodec::deserialize(packet);
}

void send_operation_response(int socket_fd, KeyStoreStatus status) {
    const std::uint8_t response = status == KeyStoreStatus::Success ? 1 : 0;
    send_all(socket_fd, &response, sizeof(response));
}

void send_get_response(int socket_fd, const KeyStoreGetResult &result) {
    if (result.status == KeyStoreStatus::KeyNotFound) {
        const std::uint32_t network_response_size = htonl(0);
        send_all(socket_fd, reinterpret_cast<const std::uint8_t *>(&network_response_size), sizeof(network_response_size));
        return;
    }

    if (result.status != KeyStoreStatus::Success || !result.value.has_value()) {
        throw std::runtime_error("[ERROR] Failed to execute get command");
    }

    const std::vector<std::uint8_t> network_value = NetCodec::serialize_value(*result.value);
    if (network_value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("[ERROR] Serialized value response is too large");
    }

    const std::uint32_t network_response_size = htonl(static_cast<std::uint32_t>(network_value.size()));
    send_all(socket_fd, reinterpret_cast<const std::uint8_t *>(&network_response_size), sizeof(network_response_size));
    send_all(socket_fd, network_value.data(), network_value.size());
}

void execute_command(int socket_fd, KeyStore &key_store, const Command &command, bool &transaction_active) {
    // The result lives in this dispatcher until its executor joins, giving the
    // worker a safe output reference without adding futures to this MVP.
    switch (command.op) {
        case Operator::GET: {
            KeyStoreGetResult result{};
            std::thread executor(handle_get, std::ref(key_store), std::ref(result), std::cref(*command.key));
            executor.join();
            send_get_response(socket_fd, result);
            return;
        }
        case Operator::PUT: {
            KeyStoreStatus result = KeyStoreStatus::WriteFailed;
            std::thread executor(handle_put, std::ref(key_store), std::ref(result), std::cref(*command.key), std::cref(*command.value));
            executor.join();
            send_operation_response(socket_fd, result);
            return;
        }
        case Operator::DELETE: {
            KeyStoreRemoveResult result{};
            std::thread executor(handle_remove, std::ref(key_store), std::ref(result), std::cref(*command.key));
            executor.join();
            send_operation_response(socket_fd, result.status);
            return;
        }
        case Operator::BEGIN_TXN: {
            KeyStoreStatus result = KeyStoreStatus::WriteFailed;
            std::thread executor(handle_begin_transaction, std::ref(key_store), std::ref(result));
            executor.join();
            if (result == KeyStoreStatus::Success) transaction_active = true;
            send_operation_response(socket_fd, result);
            return;
        }
        case Operator::COMMIT: {
            KeyStoreStatus result = KeyStoreStatus::CommitFailed;
            std::thread executor(handle_commit, std::ref(key_store), std::ref(result));
            executor.join();
            if (result == KeyStoreStatus::Success) transaction_active = false;
            send_operation_response(socket_fd, result);
            return;
        }
        case Operator::ROLLBACK: {
            KeyStoreStatus result = KeyStoreStatus::RollbackFailed;
            std::thread executor(handle_rollback, std::ref(key_store), std::ref(result));
            executor.join();
            if (result == KeyStoreStatus::Success) transaction_active = false;
            send_operation_response(socket_fd, result);
            return;
        }
    }

    throw std::runtime_error("[ERROR] Unsupported command operator");
}

void rollback_active_transaction(KeyStore &key_store, bool transaction_active) noexcept {
    if (!transaction_active) return;

    // A disconnected client cannot resolve its transaction, so finish the same
    // joined-handler flow before allowing the dispatcher to release its socket.
    try {
        KeyStoreStatus result = KeyStoreStatus::RollbackFailed;
        std::thread executor(handle_rollback, std::ref(key_store), std::ref(result));
        executor.join();
    } catch (...) {
        // Connection cleanup cannot report a recovery failure to a departed client.
    }
}

TransactionHandle command_transaction(
    SessionContext &context,
    TransactionManager &transaction_manager,
    bool &implicit
) {
    implicit = !context.active_transaction;
    return implicit
        ? transaction_manager.begin()
        : context.active_transaction;
}

void finish_implicit_transaction(
    const TransactionHandle &transaction,
    TransactionManager &transaction_manager,
    bool success
) {
    if (success) {
        if (transaction_manager.commit(transaction) != CommitStatus::Success) {
            throw std::runtime_error("[ERROR] Failed to commit implicit transaction");
        }
        return;
    }

    if (transaction_manager.abort(
            transaction,
            AbortReason::StatementFailure) != AbortStatus::Success) {
        throw std::runtime_error("[ERROR] Failed to abort implicit transaction");
    }
}

void execute_transactional_command(
    int socket_fd,
    KeyStore &key_store,
    TransactionManager &transaction_manager,
    SessionContext &context,
    const Command &command
) {
    if (command.op == Operator::BEGIN_TXN) {
        if (context.active_transaction) {
            send_operation_response(socket_fd, KeyStoreStatus::TransactionAlreadyActive);
            return;
        }
        context.active_transaction = transaction_manager.begin();
        send_operation_response(socket_fd, KeyStoreStatus::Success);
        return;
    }

    if (command.op == Operator::COMMIT) {
        if (!context.active_transaction) {
            send_operation_response(socket_fd, KeyStoreStatus::NoActiveTransaction);
            return;
        }
        const CommitStatus status = transaction_manager.commit(context.active_transaction);
        if (status == CommitStatus::Success) context.active_transaction.reset();
        send_operation_response(
            socket_fd,
            status == CommitStatus::Success
                ? KeyStoreStatus::Success
                : KeyStoreStatus::CommitFailed);
        return;
    }

    if (command.op == Operator::ROLLBACK) {
        if (!context.active_transaction) {
            send_operation_response(socket_fd, KeyStoreStatus::NoActiveTransaction);
            return;
        }
        const AbortStatus status = transaction_manager.abort(
            context.active_transaction,
            AbortReason::ClientRequest);
        if (status == AbortStatus::Success) context.active_transaction.reset();
        send_operation_response(
            socket_fd,
            status == AbortStatus::Success
                ? KeyStoreStatus::Success
                : KeyStoreStatus::RollbackFailed);
        return;
    }

    bool implicit = false;
    const TransactionHandle transaction = command_transaction(
        context,
        transaction_manager,
        implicit);

    if (command.op == Operator::GET) {
        KeyStoreGetResult result = key_store.get(transaction, *command.key);
        const bool success = result.status == KeyStoreStatus::Success ||
            result.status == KeyStoreStatus::KeyNotFound;
        if (implicit) finish_implicit_transaction(transaction, transaction_manager, success);
        if (!implicit && result.status == KeyStoreStatus::Deadlock) {
            transaction_manager.abort(transaction, AbortReason::DeadlockVictim);
            context.active_transaction.reset();
        }
        send_get_response(socket_fd, result);
        return;
    }

    if (command.op == Operator::PUT) {
        KeyStoreStatus result = key_store.put(
            transaction,
            *command.key,
            *command.value);
        if (implicit) {
            finish_implicit_transaction(
                transaction,
                transaction_manager,
                result == KeyStoreStatus::Success);
        } else if (result == KeyStoreStatus::Deadlock) {
            transaction_manager.abort(transaction, AbortReason::DeadlockVictim);
            context.active_transaction.reset();
        }
        send_operation_response(socket_fd, result);
        return;
    }

    KeyStoreRemoveResult result = key_store.remove(transaction, *command.key);
    const bool success = result.status == KeyStoreStatus::Success ||
        result.status == KeyStoreStatus::KeyNotFound;
    if (implicit) {
        finish_implicit_transaction(transaction, transaction_manager, success);
    } else if (result.status == KeyStoreStatus::Deadlock) {
        transaction_manager.abort(transaction, AbortReason::DeadlockVictim);
        context.active_transaction.reset();
    }
    send_operation_response(socket_fd, result.status);
}

} // namespace

void handle_get(KeyStore &key_store, KeyStoreGetResult &result, const Key &key) {
    result = key_store.get(key);
}

void handle_put(KeyStore &key_store, KeyStoreStatus &result, const Key &key, const Value &value) {
    result = key_store.put(key, value);
}

void handle_remove(KeyStore &key_store, KeyStoreRemoveResult &result, const Key &key) {
    result = key_store.remove(key);
}

void handle_begin_transaction(KeyStore &key_store, KeyStoreStatus &result) {
    result = key_store.begin_write_transaction();
}

void handle_commit(KeyStore &key_store, KeyStoreStatus &result) {
    result = key_store.commit();
}

void handle_rollback(KeyStore &key_store, KeyStoreStatus &result) {
    result = key_store.rollback();
}

void serve_connection(int socket_fd, KeyStore &key_store) noexcept {
    bool transaction_active = false;

    try {
        while (true) {
            const Command command = read_command(socket_fd);
            execute_command(socket_fd, key_store, command, transaction_active);
        }
    } catch (...) {
        rollback_active_transaction(key_store, transaction_active);
        ::close(socket_fd);
    }
}

void serve_transactional_connection(
    int socket_fd,
    KeyStore &key_store,
    TransactionManager &transaction_manager
) noexcept {
    SessionContext context{};

    try {
        while (true) {
            const Command command = read_command(socket_fd);
            execute_transactional_command(
                socket_fd,
                key_store,
                transaction_manager,
                context,
                command);
        }
    } catch (...) {
        if (context.active_transaction) {
            try {
                transaction_manager.abort(
                    context.active_transaction,
                    AbortReason::ClientRequest);
            } catch (...) {
            }
        }
        ::close(socket_fd);
    }
}

} // namespace CommandServer
