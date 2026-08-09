#include <gtest/gtest.h>

#include <Command.h>
#include <NetCodec.h>
#include <Session.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

void send_all_for_test(int fd, const std::uint8_t *buffer, std::size_t size) {
    std::size_t bytes_sent = 0;

    while (bytes_sent < size) {
        const ssize_t result = ::send(fd, buffer + bytes_sent, size - bytes_sent, 0);
        if (result > 0) {
            bytes_sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::runtime_error("Failed to send test bytes");
    }
}

void receive_all_for_test(int fd, std::uint8_t *buffer, std::size_t size) {
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
        throw std::runtime_error("Failed to receive test bytes");
    }
}

Command receive_command(int fd) {
    std::uint32_t network_payload_size = 0;
    receive_all_for_test(fd, reinterpret_cast<std::uint8_t *>(&network_payload_size), sizeof(network_payload_size));
    const std::uint32_t host_payload_size = ntohl(network_payload_size);

    std::vector<std::uint8_t> packet(sizeof(network_payload_size) + host_payload_size);
    std::memcpy(packet.data(), &network_payload_size, sizeof(network_payload_size));
    receive_all_for_test(fd, packet.data() + sizeof(network_payload_size), host_payload_size);
    return CommandCodec::deserialize(packet);
}

void send_get_response(int fd, const std::vector<std::uint8_t> &network_value) {
    const std::uint32_t network_size = htonl(static_cast<std::uint32_t>(network_value.size()));
    send_all_for_test(fd, reinterpret_cast<const std::uint8_t *>(&network_size), sizeof(network_size));
    send_all_for_test(fd, network_value.data(), network_value.size());
}

void send_operation_response(int fd, std::uint8_t status) {
    send_all_for_test(fd, &status, sizeof(status));
}

TEST(SessionTest, SendsPutRemoveAndTransactionCommands) {
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

    Session session(sockets[0], 17);
    EXPECT_EQ(session.get_id(), 17);

    send_operation_response(sockets[1], 1);
    session.put(KeyInput{std::uint64_t{42}}, ValueInput{std::string{"value"}});
    const Command put = receive_command(sockets[1]);
    EXPECT_EQ(put.operator_type, OperatorType::BINARY);
    EXPECT_EQ(put.op, Operator::PUT);
    ASSERT_TRUE(put.key.has_value());
    ASSERT_TRUE(put.value.has_value());
    EXPECT_TRUE(KeyCodec::equal(*put.key, KeyCodec::make_uint64(42)));
    EXPECT_TRUE(ValueCodec::equal(*put.value, ValueCodec::make_char("value")));

    send_operation_response(sockets[1], 1);
    session.remove(KeyInput{std::string{"key"}});
    const Command remove = receive_command(sockets[1]);
    EXPECT_EQ(remove.operator_type, OperatorType::UNARY);
    EXPECT_EQ(remove.op, Operator::DELETE);
    ASSERT_TRUE(remove.key.has_value());
    EXPECT_TRUE(KeyCodec::equal(*remove.key, KeyCodec::make_string("key")));

    send_operation_response(sockets[1], 1);
    session.begin_transaction();
    EXPECT_EQ(receive_command(sockets[1]).op, Operator::BEGIN_TXN);
    send_operation_response(sockets[1], 1);
    session.commit();
    EXPECT_EQ(receive_command(sockets[1]).op, Operator::COMMIT);
    send_operation_response(sockets[1], 1);
    session.rollback();
    EXPECT_EQ(receive_command(sockets[1]).op, Operator::ROLLBACK);

    ::close(sockets[1]);
}

TEST(SessionTest, GetReturnsDecodedValue) {
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

    Session session(sockets[0], 1);
    send_get_response(sockets[1], NetCodec::serialize_value(ValueCodec::make_char("answer")));

    const std::optional<ValueInput> result = session.get(KeyInput{std::string{"key"}});
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<std::string>(*result));
    EXPECT_EQ(std::get<std::string>(*result), "answer");

    const Command get = receive_command(sockets[1]);
    EXPECT_EQ(get.operator_type, OperatorType::UNARY);
    EXPECT_EQ(get.op, Operator::GET);
    ASSERT_TRUE(get.key.has_value());
    EXPECT_TRUE(KeyCodec::equal(*get.key, KeyCodec::make_string("key")));

    ::close(sockets[1]);
}

TEST(SessionTest, GetReturnsNulloptForZeroLengthResponse) {
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

    Session session(sockets[0], 2);
    const std::uint32_t not_found = 0;
    send_all_for_test(sockets[1], reinterpret_cast<const std::uint8_t *>(&not_found), sizeof(not_found));

    EXPECT_FALSE(session.get(KeyInput{std::uint64_t{7}}).has_value());
    EXPECT_EQ(receive_command(sockets[1]).op, Operator::GET);

    ::close(sockets[1]);
}

TEST(SessionTest, CloseIsIdempotentAndDestructorClosesSocket) {
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

    const int owned_fd = sockets[0];
    {
        Session session(owned_fd, 3);
        session.close();
        session.close();
        EXPECT_EQ(::fcntl(owned_fd, F_GETFD), -1);
    }

    EXPECT_EQ(::fcntl(owned_fd, F_GETFD), -1);
    ::close(sockets[1]);

    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    const int destructor_fd = sockets[0];
    {
        Session session(destructor_fd, 4);
    }

    EXPECT_EQ(::fcntl(destructor_fd, F_GETFD), -1);
    ::close(sockets[1]);
}

TEST(SessionTest, OperationsOnClosedSessionThrow) {
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

    Session session(sockets[0], 5);
    session.close();

    EXPECT_THROW(session.commit(), std::runtime_error);
    EXPECT_THROW(session.get(KeyInput{std::uint64_t{1}}), std::runtime_error);
    ::close(sockets[1]);
}

TEST(SessionTest, OperationResponsesReportFailureAndRejectInvalidStatus) {
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

    Session session(sockets[0], 6);

    send_operation_response(sockets[1], 0);
    EXPECT_THROW(session.commit(), std::runtime_error);
    EXPECT_EQ(receive_command(sockets[1]).op, Operator::COMMIT);

    send_operation_response(sockets[1], 1);
    EXPECT_NO_THROW(session.rollback());
    EXPECT_EQ(receive_command(sockets[1]).op, Operator::ROLLBACK);

    send_operation_response(sockets[1], 2);
    EXPECT_THROW(session.begin_transaction(), std::runtime_error);
    EXPECT_EQ(receive_command(sockets[1]).op, Operator::BEGIN_TXN);
    EXPECT_THROW(session.commit(), std::runtime_error);

    ::close(sockets[1]);
}

} // namespace
