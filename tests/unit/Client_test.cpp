#include <gtest/gtest.h>

#include <Client.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>

namespace {

struct ListeningSocket {
    int fd;
    std::uint16_t port;

    ListeningSocket(): fd(::socket(AF_INET, SOCK_STREAM, 0)), port(0) {
        if (fd < 0) {
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;

        if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(fd, 2) != 0) {
            ::close(fd);
            fd = -1;
            return;
        }

        socklen_t address_size = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &address_size) != 0) {
            ::close(fd);
            fd = -1;
            return;
        }

        port = ntohs(address.sin_port);
    }

    ~ListeningSocket() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

TEST(ClientTest, CloseSessionClosesItsSocket) {
    ListeningSocket listener;
    ASSERT_GE(listener.fd, 0);

    Client client("127.0.0.1", listener.port);
    Session &session = client.new_session();
    const int accepted_fd = ::accept(listener.fd, nullptr, nullptr);
    ASSERT_GE(accepted_fd, 0);

    client.close_session(session.get_id());

    std::uint8_t byte = 0;
    EXPECT_EQ(::recv(accepted_fd, &byte, sizeof(byte), 0), 0);
    ::close(accepted_fd);
}

TEST(ClientTest, DestructorClosesEverySessionSocket) {
    ListeningSocket listener;
    ASSERT_GE(listener.fd, 0);

    int first_fd = -1;
    int second_fd = -1;
    {
        Client client("127.0.0.1", listener.port);
        client.new_session();
        first_fd = ::accept(listener.fd, nullptr, nullptr);
        ASSERT_GE(first_fd, 0);

        client.new_session();
        second_fd = ::accept(listener.fd, nullptr, nullptr);
        ASSERT_GE(second_fd, 0);
    }

    std::uint8_t byte = 0;
    EXPECT_EQ(::recv(first_fd, &byte, sizeof(byte), 0), 0);
    EXPECT_EQ(::recv(second_fd, &byte, sizeof(byte), 0), 0);
    ::close(first_fd);
    ::close(second_fd);
}

} // namespace
