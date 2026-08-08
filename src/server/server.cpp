#include <sys/socket.h> 
#include <netinet/in.h>
#include <cstdlib>

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

    while (true) {
        struct sockaddr_in addr{};
        socklen_t size = sizeof(addr);
        int socket_fd = accept(fd, reinterpret_cast<sockaddr *>(&addr), &size);
        if (socket_fd == -1) {
            continue;
        }
    }
}