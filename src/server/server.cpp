#include <KeyStore.h>
#include <server/CommandServer.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr std::uint16_t SERVER_PORT = 8080;

int create_listener() {
    // Keep socket setup in one place so every startup failure closes the
    // partially initialized descriptor before returning to main.
    const int listener_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd < 0) return -1;

    const int reuse_address = 1;
    if (::setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) != 0) {
        ::close(listener_fd);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(SERVER_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(listener_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(listener_fd);
        return -1;
    }

    if (::listen(listener_fd, SOMAXCONN) != 0) {
        ::close(listener_fd);
        return -1;
    }

    return listener_fd;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <db-file>" << std::endl;
        return 1;
    }

    KeyStore key_store;
    if (key_store.open(std::string{argv[1]}) != KeyStoreStatus::Success) {
        std::cerr << "[ERROR] Failed to open database: " << argv[1] << std::endl;
        return 1;
    }

    const int listener_fd = create_listener();
    if (listener_fd < 0) {
        std::cerr << "[ERROR] Failed to listen on 127.0.0.1:" << SERVER_PORT << std::endl;
        return 1;
    }

    std::cout << "Listening on 127.0.0.1:" << SERVER_PORT << std::endl;

    while (true) {
        sockaddr_in client_address{};
        socklen_t client_address_size = sizeof(client_address);
        const int socket_fd = ::accept(listener_fd, reinterpret_cast<sockaddr *>(&client_address), &client_address_size);
        if (socket_fd < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[ERROR] Failed to accept client connection" << std::endl;
            continue;
        }

        // The dispatcher owns the accepted descriptor. Its executor threads
        // remain joined, while this connection-level thread runs independently.
        try {
            std::thread dispatcher(CommandServer::serve_connection, socket_fd, std::ref(key_store));
            dispatcher.detach();
        } catch (...) {
            ::close(socket_fd);
        }
    }
}
