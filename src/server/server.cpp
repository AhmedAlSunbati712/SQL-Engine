#include <sys/socket.h> 
#include <netinet/in.h>
#include <cstdlib>
#include <memory>
#include <thread>
#include <Message.h>
#include <vector>
#include <iostream>
#include <unistd.h>

constexpr std::uint32_t MAX_PAYLOAD_SIZE = 16 * 1024 * 1024; 

struct Stats {
    int messages_received;
};

void handle_client(int socket_, std::shared_ptr<Stats> connection_stats);
std::uint32_t read_payload_size(int socket_);
std::vector<std::uint8_t> read_payload(int socket_, std::uint32_t payload_size);

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt_val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, static_cast<socklen_t>(sizeof(opt_val)));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0);
    int rv = bind(fd, reinterpret_cast<const sockaddr *>(&addr), static_cast<socklen_t>(sizeof(addr)));
    if (rv == -1) {
        std::exit(1);
    }
    
    rv = listen(fd, 0);
    if (rv == -1) {
        std::exit(1);
    }

    std::cout << "======== Listening on " << addr.sin_addr.s_addr << ":" << addr.sin_port << " ========" << std::endl;

    while (true) {
        struct sockaddr_in addr{};
        socklen_t size = sizeof(addr);
        int socket_fd = accept(fd, reinterpret_cast<sockaddr *>(&addr), &size);
        if (socket_fd == -1) {
            continue;
        }
        std::cout << "======== Accepted connection from " << addr.sin_addr.s_addr << ":" << addr.sin_port << " ========" << std::endl;
        std::shared_ptr<Stats> connection_stats = std::make_shared<Stats>();
        std::thread t(handle_client, socket_fd, connection_stats);
        t.detach();
    }
}

void handle_client(int socket_, std::shared_ptr<Stats> connection_stats) {
    while (true) {
        try {
            std::uint32_t payload_size = read_payload_size(socket_);
            std::vector<std::uint8_t> payload = read_payload(socket_, payload_size);
            Message msg = MessageCodec::deserialize(payload);
            std::cout << "The client said: " << msg.text << std::endl;
            connection_stats->messages_received += 1;
        } catch (const std::exception& e) {   // catches runtime_error, bad_alloc, etc.
            close(socket_);
            return;
        }
    }
}

std::uint32_t read_payload_size(int socket_) {
    char buffer[4];
    size_t bytes_read = 0;

    while (bytes_read < 4) {
        ssize_t result = recv(socket_, buffer + bytes_read, 4 - bytes_read, 0);
        if (result == 0) {
            throw std::runtime_error("Connection closed by remote !\n");
        } else if (result > 0){
            bytes_read += result;
        } else {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("Socket error occured!\n");
        }
    }
    std::uint32_t network_payload_size;
    std::memcpy(&network_payload_size, buffer, bytes_read);
    std::uint32_t host_payload_size = ntohl(network_payload_size);
    return host_payload_size;
}

std::vector<std::uint8_t> read_payload(int socket_, std::uint32_t payload_size) {
    if (payload_size > MAX_PAYLOAD_SIZE) {
        throw std::runtime_error("Payload too large!");
    }
    std::vector<std::uint8_t> buffer{};
    buffer.resize(payload_size);

    size_t bytes_read = 0;
    while (bytes_read < payload_size) {
        int result = recv(socket_, buffer.data() + bytes_read, payload_size - bytes_read, 0);
                if (result == 0) {
            throw std::runtime_error("Connection closed by remote !\n");
        } else if (result > 0){
            bytes_read += result;
        } else {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("Socket error occured!\n");
        }
    }
    return buffer;
}