#include <Client.h>
#include <cstring>      // For std::memset
#include <arpa/inet.h>  // For sockaddr_in, inet_pton, htons
#include <unistd.h>
#include <sys/socket.h>
#include <stdexcept>

Client::Client(std::string ip_addr, int port): seq_number(0) {
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<std::uint16_t>(port));

    if (inet_pton(AF_INET, ip_addr.c_str(), &server_addr.sin_addr) != 1) {
        throw std::runtime_error("[ERROR] Misformatted ip address");
    }
}

Client::~Client() = default;

Session &Client::new_session() {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        throw std::runtime_error("[ERROR] Failed to create socket!");
    }

    int opt_val = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val)) != 0) {
        close(socket_fd);
        throw std::runtime_error("[ERROR] Failed to set socket options!");
    }

    if (connect(socket_fd, reinterpret_cast<const sockaddr *>(&server_addr), sizeof(server_addr)) < 0) {
        close(socket_fd);
        throw std::runtime_error("[ERROR] Failed to connect to server!");
    }

    int id = seq_number++;
    auto [it, inserted] = sessions.try_emplace(id, socket_fd, id);
    return it->second;
}

void Client::close_session(int session_id) {
    sessions.erase(session_id);
}
